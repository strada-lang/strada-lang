#!/bin/bash
# t_audit_r19.sh - Cycle 19 regression tests across compiler, runtime, stdlib/C-exts, Eval/VM/JIT

r19_dir="$BUILD_DIR/audit_r19"
mkdir -p "$r19_dir"

r19_record() {
    local rc="$1"
    local label="$2"
    local diag="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "$label"
    else
        FAILED=$((FAILED + 1))
        log_fail "$label" "$diag"
    fi
}

# 1. Compiler, Runtime & Stdlib suite (-O0 by default, plus -O2 under --default)
for opt in ${STRADA_TEST_AUDIT_OPTS:--O0}; do
    out=$("$PROJECT_DIR/strada" "$opt" -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r19/compiler_runtime.strada" -lpthread -o "$r19_dir/comp_${opt}" 2>&1 \
        && timeout 20 "$r19_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R19_COMPILER_RUNTIME_OK"
    r19_record $? "Cycle 19 Compiler, Runtime & Stdlib suite ($opt)" "rc=$rc out=$out"
done

# 2. L1 & L2: lib/perl5 UV preservation + scoped eval_pv temp cleanup
p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts 2>/dev/null || true)
if [ -n "$p5_flags" ]; then
    cat > "$r19_dir/l1_l2_perl5.c" << 'EOF'
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strada_runtime.h"
extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_run_raw(const char *code);
extern char* strada_perl5_eval_raw(const char *code);
extern int strada_perl5_use_raw(const char *module);
extern int strada_perl5_require_raw(const char *module);
int main(void) {
    assert(strada_perl5_init() == 1);
    /* Verify repeated eval_pv calls inside estrada_*_raw do not blow PL_tmps_stack */
    for (int i = 0; i < 500; i++) {
        strada_perl5_run_raw("my $s = 'x' x 64;");
        char *v = strada_perl5_eval_raw("1 + 2");
        assert(strcmp(v, "3") == 0);
        free(v);
    }
    assert(strada_perl5_use_raw("strict") == 1);
    assert(strada_perl5_require_raw("Exporter") == 1);
    strada_perl5_shutdown();
    printf("L2_PERL5_C_OK\n");
    return 0;
}
EOF
    cat > "$r19_dir/l1_l2_perl5.strada" << EOF
use lib "$PROJECT_DIR/lib";
use perl5;
func main() int {
    perl5::init();
    perl5::eval("sub r19_echo { return \$_[0]; }");
    # ++ past INT64_MAX is how Strada makes a UV (STRADA_SVf_UV).
    my scalar \$u = 9223372036854775807;
    \$u++;
    my str \$got = perl5::call("r19_echo", \$u);
    if (\$got ne "9223372036854775808") {
        say("BAD UV got=" . \$got);
        return 1;
    }
    if (perl5::use_module("strict") != 1 || perl5::require_module("Exporter") != 1) {
        say("BAD use/require");
        return 2;
    }
    perl5::shutdown();
    say("L1_L2_PERL5_STRADA_OK");
    return 0;
}
EOF
    p5_out=$(
        $GCC -O0 -I"$PROJECT_DIR/runtime" "$r19_dir/l1_l2_perl5.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" \
            "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS $p5_flags -o "$r19_dir/l1_l2_perl5_c" 2>&1 \
            && timeout 20 "$r19_dir/l1_l2_perl5_c" 2>&1 \
            && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r19_dir/l1_l2_perl5.strada" $p5_flags -o "$r19_dir/l1_l2_perl5_strada" 2>&1 \
            && timeout 20 "$r19_dir/l1_l2_perl5_strada" 2>&1
    )
    p5_rc=$?
    [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "L2_PERL5_C_OK" && printf '%s\n' "$p5_out" | grep -qx "L1_L2_PERL5_STRADA_OK"
    r19_record $? "L1/L2 lib/perl5 UV call preservation & scoped eval_pv" "rc=$p5_rc out=$p5_out"
fi

# 3. Eval / VM / JIT / Worker STW GC suite (V1-V8)
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r19/eval_jit.strada" -lpthread -o "$r19_dir/eval_jit" 2>&1 \
    && STRADA_HOME="$PROJECT_DIR" timeout 30 "$r19_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R19_EVAL_JIT_OK"
r19_record $? "Cycle 19 Eval / VM / JIT / Worker STW GC suite (V1-V8)" "rc=$ej_rc out=$ej_out"

# 4. V5 CLI -e multi-package segment check
mp_e_out=$(timeout 20 "$PROJECT_DIR/interpreter/strada-interp" -e 'package PA; our str $a = __PACKAGE__; func get_a() str { return $a; } package PB; my str $b = __PACKAGE__; say(PA::get_a() . ":" . $b); package PC; say(__PACKAGE__);' 2>&1)
mp_e_rc=$?
[ "$mp_e_rc" -eq 0 ] && [ "$mp_e_out" = $'PA:PB\nPC' ]
r19_record $? "V5 CLI -e multi-package body scoping" "rc=$mp_e_rc out='$mp_e_out'"

# 5. Compiled vs bytecode VM 2-way parity (test_audit_r19_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r19_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r19_vm.expected")
p_comp_err=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$par_src" -o "$r19_dir/par_comp" 2>&1)
p_comp_rc=$?
if [ "$p_comp_rc" -eq 0 ]; then
    p_comp=$(timeout 20 "$r19_dir/par_comp" 2>&1)
else
    p_comp="$p_comp_err"
fi
p_vm=$(timeout 20 "$PROJECT_DIR/interpreter/strada-interp" -L "$PROJECT_DIR/lib" "$par_src" 2>&1)
[ "$p_comp_rc" -eq 0 ] && [ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r19_record $? "Cycle 19 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"
