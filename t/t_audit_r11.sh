#!/usr/bin/env bash
# t_audit_r11.sh — Cycle 11 Audit regression suite

aud_dir="$BUILD_DIR/audit_r11"
aud_src="$SCRIPT_DIR/audit_r11"
mkdir -p "$aud_dir"

aud_r11_record() {
    local status=$1
    local name="$2"
    local diag="${3:-}"
    TOTAL=$((TOTAL + 1))
    if [ "$status" -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "$name"
    else
        FAILED=$((FAILED + 1))
        log_fail "$name" "$diag"
    fi
}

aud_r11_compile_run() {
    local src="$1"
    local tag="$2"
    local expect="$3"
    shift 3
    local bin="$aud_dir/$tag"
    local log="$aud_dir/$tag.build.log"
    local rc=1
    local out=""
    if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" "$src" "$@" -o "$bin" >"$log" 2>&1); then
        out=$(timeout 30 "$bin" 2>&1) && rc=0 || rc=$?
    else
        out=$(tail -n 20 "$log" 2>/dev/null || true)
    fi
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "$expect"; then
        return 0
    fi
    printf 'rc=%s out=%s\n' "$rc" "$out" > "$aud_dir/$tag.diag"
    return 1
}

# 1. Runtime & Compiler/Lexer suite at -O0 and -O2 (R11-C1..C14, R11-RT1/4/5/6)
if aud_r11_compile_run "$aud_src/runtime_compiler.strada" "r11_rc_O0" "R11_RUNTIME_COMPILER_OK" -O0 -I "$aud_src/mods"; then
    aud_r11_record 0 "Cycle 11 runtime/compiler/lexer suite (-O0)" ""
else
    aud_r11_record 1 "Cycle 11 runtime/compiler/lexer suite (-O0)" "$(cat "$aud_dir/r11_rc_O0.diag" 2>/dev/null)"
fi

if aud_r11_compile_run "$aud_src/runtime_compiler.strada" "r11_rc_O2" "R11_RUNTIME_COMPILER_OK" -O2 -I "$aud_src/mods"; then
    aud_r11_record 0 "Cycle 11 runtime/compiler/lexer suite (-O2)" ""
else
    aud_r11_record 1 "Cycle 11 runtime/compiler/lexer suite (-O2)" "$(cat "$aud_dir/r11_rc_O2.diag" 2>/dev/null)"
fi

# 2. Stdlib suite (JSON canonical unwind + Gettext invalid set_locale)
if aud_r11_compile_run "$aud_src/stdlib_tools.strada" "r11_stdlib" "R11_STDLIB_OK" -O0 "$PROJECT_DIR/lib/Gettext/gettext_wrapper.c"; then
    aud_r11_record 0 "Cycle 11 stdlib suite (JSON, Gettext)" ""
else
    aud_r11_record 1 "Cycle 11 stdlib suite (JSON, Gettext)" "$(cat "$aud_dir/r11_stdlib.diag" 2>/dev/null)"
fi

# 3. R11-LIB1: lib/perl5.strada & lib/perl5/strada_perl5.c marshaling (tagged int + blessed overload)
if command -v perl >/dev/null 2>&1 && perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)

    cat > "$aud_dir/r11_p5_strada.strada" <<EOF
use lib "$PROJECT_DIR/lib";
use perl5;

package R11P5Str;
use overload '""' => "to_s";
func new(str \$v) scalar { return bless(\$v, "R11P5Str"); }
func to_s(scalar \$self) str { return "ok"; }

package R11P5TieInt;
func TIESCALAR(str \$cls, int \$v) scalar { return bless({"v" => \$v}, \$cls); }
func FETCH(scalar \$self) scalar { return \$self->{"v"}; }
func STORE(scalar \$self, scalar \$v) void { \$self->{"v"} = \$v; }

package main;
func main() int {
    if (perl5::init() != 1) { say("FAIL init"); return 1; }
    perl5::run('sub r11_echo { return \$_[0]; } sub r11_len { return length(\$_[0]); }');
    my str \$long_raw = "A" x 200;
    my scalar \$bl = R11P5Str::new(\$long_raw);
    perl5::set_var("r11_bl", \$bl);
    my str \$g1 = perl5::get_var("r11_bl");
    my str \$c1 = perl5::call("r11_echo", \$bl);
    my str \$l1 = perl5::call("r11_len", \$bl);

    my scalar \$ts = "";
    tie(\$ts, "R11P5TieInt", 42);
    perl5::set_var("r11_ti", \$ts);
    my str \$g2 = perl5::get_var("r11_ti");
    my str \$c2 = perl5::call("r11_echo", \$ts);
    untie(\$ts);
    perl5::shutdown();
    if (\$g1 ne "ok" || \$c1 ne "ok" || (\$l1 + 0) != 2 || \$g2 ne "42" || \$c2 ne "42") {
        say("FAIL g1=" . \$g1 . " c1=" . \$c1 . " l1=" . \$l1 . " g2=" . \$g2 . " c2=" . \$c2);
        return 2;
    }
    say("R11_P5_STRADA_OK");
    return 0;
}
EOF
    p5_rc=1; p5_out=""
    if "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/r11_p5_strada.strada" $p5_flags -o "$aud_dir/r11_p5_strada" > "$aud_dir/r11_p5_strada.log" 2>&1; then
        p5_out=$(timeout 20 "$aud_dir/r11_p5_strada" 2>&1) && p5_rc=0 || p5_rc=$?
    else
        p5_out=$(tail -n 15 "$aud_dir/r11_p5_strada.log" 2>/dev/null || true)
    fi
    if [ "$p5_rc" -eq 0 ] && echo "$p5_out" | grep -q "R11_P5_STRADA_OK"; then
        aud_r11_record 0 "R11-LIB1 (lib/perl5.strada): blessed str overload & tied int scalar" ""
    else
        aud_r11_record 1 "R11-LIB1 (lib/perl5.strada)" "rc=$p5_rc out=$p5_out"
    fi

    cat > "$aud_dir/r11_p5_c_test.c" <<'EOF'
#include "strada_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_set_scalar(StradaValue *name_sv, StradaValue *value_sv);
extern StradaValue* strada_perl5_get_scalar_sv(StradaValue *name_sv);

static StradaValue* p5t_tiescalar(StradaValue *cls, StradaValue *args) {
    (void)args;
    const char *pkg = (cls && !STRADA_IS_TAGGED_INT(cls) && cls->type == STRADA_STR && cls->value.pv) ? cls->value.pv : "R11CP5Tie";
    return strada_bless(strada_new_hash(), pkg);
}
static StradaValue* p5t_fetch(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return STRADA_MAKE_TAGGED_INT(42);
}
static StradaValue* p5t_str_ov(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_new_str("ok");
}

int main(void) {
    strada_oop_init();
    strada_method_register("R11CP5Tie", "TIESCALAR", p5t_tiescalar);
    strada_method_register("R11CP5Tie", "FETCH", p5t_fetch);
    strada_overload_register("R11CP5Ov", "\"\"", p5t_str_ov);
    assert(strada_perl5_init() == 1);

    StradaValue *ts = strada_new_undef();
    StradaValue *tref = strada_new_ref(ts, '$');
    strada_decref(strada_tie_scalar(tref, "R11CP5Tie", 0));
    strada_decref(tref);
    StradaValue *n1 = strada_new_str("r11c_ti");
    strada_perl5_set_scalar(n1, ts);
    StradaValue *g1 = strada_perl5_get_scalar_sv(n1);
    assert(g1 && g1->type == STRADA_STR && strcmp(g1->value.pv, "42") == 0);
    strada_decref(g1);
    strada_decref(n1);
    strada_decref(ts);

    char big[201];
    memset(big, 'Z', 200);
    big[200] = '\0';
    StradaValue *bstr = strada_bless(strada_new_str(big), "R11CP5Ov");
    StradaValue *n2 = strada_new_str("r11c_bl");
    strada_perl5_set_scalar(n2, bstr);
    StradaValue *g2 = strada_perl5_get_scalar_sv(n2);
    assert(g2 && g2->type == STRADA_STR && strcmp(g2->value.pv, "ok") == 0 && STRADA_STR_BYTELEN(g2) == 2);
    strada_decref(g2);
    strada_decref(n2);
    strada_decref(bstr);

    strada_perl5_shutdown();
    printf("R11_P5_C_OK\n");
    return 0;
}
EOF
    p5c_rc=1; p5c_out=""
    if gcc -O0 -I"$PROJECT_DIR/runtime" -DHAVE_PCRE2 -DPCRE2_STATIC -I"$PROJECT_DIR/vendor/pcre2/src" \
        -DSTRADA_CYCLE_GC -DSTRADA_ARENA -DSTRADA_HAVE_EPOLL \
        "$aud_dir/r11_p5_c_test.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" "$PROJECT_DIR/runtime/strada_runtime.c" \
        $p5_flags "$PROJECT_DIR/vendor/pcre2/libpcre2-8.a" -lm -o "$aud_dir/r11_p5_c_test" > "$aud_dir/r11_p5_c.log" 2>&1; then
        p5c_out=$(timeout 20 "$aud_dir/r11_p5_c_test" 2>&1) && p5c_rc=0 || p5c_rc=$?
    else
        p5c_out=$(tail -n 15 "$aud_dir/r11_p5_c.log" 2>/dev/null || true)
    fi
    if [ "$p5c_rc" -eq 0 ] && echo "$p5c_out" | grep -q "R11_P5_C_OK"; then
        aud_r11_record 0 "R11-LIB1 (strada_perl5.c): tied tagged-int & blessed overload marshaling" ""
    else
        aud_r11_record 1 "R11-LIB1 (strada_perl5.c)" "rc=$p5c_rc out=$p5c_out"
    fi
fi

# 4. Eval + VM + Strada::JIT suite (R11-VM1..VM7, R11-VM11)
if aud_r11_compile_run "$aud_src/eval_jit.strada" "r11_eval_jit" "R11_EVAL_JIT_OK"; then
    aud_r11_record 0 "Cycle 11 Eval / VM / Strada::JIT suite" ""
else
    aud_r11_record 1 "Cycle 11 Eval / VM / Strada::JIT suite" "$(cat "$aud_dir/r11_eval_jit.diag" 2>/dev/null)"
fi

# 5. Compiled vs bytecode VM 2-way parity (R11-VM1..VM6, R11-VM8..VM10)
parity_src="$PROJECT_DIR/interpreter/test/test_audit_r11_vm.strada"
parity_exp="$PROJECT_DIR/interpreter/test/test_audit_r11_vm.expected"
interp_bin="$PROJECT_DIR/interpreter/strada-interp"
if "$PROJECT_DIR/strada" -O0 "$parity_src" -o "$aud_dir/r11_parity" > "$aud_dir/r11_parity.build.log" 2>&1; then
    c_out=$(timeout 20 "$aud_dir/r11_parity" 2>&1)
    c_rc=$?
    vm_out=$(timeout 20 "$interp_bin" "$parity_src" 2>&1)
    vm_rc=$?
    exp_out=$(cat "$parity_exp")
    if [ "$c_rc" -eq 0 ] && [ "$vm_rc" -eq 0 ] \
       && [ "$c_out" = "$exp_out" ] && [ "$vm_out" = "$exp_out" ]; then
        aud_r11_record 0 "Cycle 11 2-way parity (compiled / VM)" ""
    else
        aud_r11_record 1 "Cycle 11 2-way parity" "c=$c_rc vm=$vm_rc c_diff=[$(diff <(echo "$exp_out") <(echo "$c_out") | head -n 6)] vm_diff=[$(diff <(echo "$exp_out") <(echo "$vm_out") | head -n 6)]"
    fi
else
    aud_r11_record 1 "Cycle 11 2-way parity compile" "$(tail -n 15 "$aud_dir/r11_parity.build.log" 2>/dev/null)"
fi

# 6. CLI checks: R11-VM7 (VM rejects undeclared vars), R11-C14 (C-keyword params in main), R11-C8 (stradapp s/// tr///)
vm7_ok=0
vm7_out1=$("$interp_bin" --vm -e 'say($vx + 1);' 2>&1) && vm7_rc1=0 || vm7_rc1=$?
vm7_out2=$("$interp_bin" --vm -e '$typo = 42; say("ok");' 2>&1) && vm7_rc2=0 || vm7_rc2=$?
if [ "$vm7_rc1" -ne 0 ] && [ "$vm7_rc2" -ne 0 ] && echo "$vm7_out1$vm7_out2" | grep -qi "undeclared"; then
    vm7_ok=1
fi
if [ "$vm7_ok" -eq 1 ]; then
    aud_r11_record 0 "R11-VM7 VM CLI rejects undeclared variables on read and write" ""
else
    aud_r11_record 1 "R11-VM7 VM CLI rejects undeclared variables" "rc1=$vm7_rc1 ($vm7_out1) rc2=$vm7_rc2 ($vm7_out2)"
fi

cat > "$aud_dir/r11_c14_kw.strada" <<'EOF'
func main(int $int, array @char) int {
    my scalar $f = func () int {
        $int = $int + 5;
        return $int;
    };
    my int $r = $f->();
    if ($r == $int && $int >= 6 && size(@char) >= 1) {
        say("R11_C14_KW_OK");
        return 0;
    }
    return 1;
}
EOF
if aud_r11_compile_run "$aud_dir/r11_c14_kw.strada" "r11_c14_kw" "R11_C14_KW_OK" -O0; then
    aud_r11_record 0 "R11-C14 main(int \$int, array @char) C-keyword params & closure box" ""
else
    aud_r11_record 1 "R11-C14 C-keyword params in main" "$(cat "$aud_dir/r11_c14_kw.diag" 2>/dev/null)"
fi

cat > "$aud_dir/pp_c8.strada" <<'EOF'
func main() int {
    my str $a = "hello";
    $a =~ s/o/"/;
    my str $b = "cat";
    $b =~ tr/a/'/;
#ifdef R11_UNDEF_PP_MACRO
    say("SHOULD_NOT_APPEAR");
#endif
    say("PP_OK:" . $a . ":" . $b);
    return 0;
}
EOF
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_c8.strada" > "$aud_dir/pp_c8.out.strada" 2>"$aud_dir/pp_c8.log" \
   && ! grep -q "SHOULD_NOT_APPEAR" "$aud_dir/pp_c8.out.strada" \
   && aud_r11_compile_run "$aud_dir/pp_c8.out.strada" "r11_pp_c8_bin" "PP_OK" -O0; then
    aud_r11_record 0 "R11-C8 tools/stradapp handles s/// & tr/// embedded quotes before #ifdef" ""
else
    aud_r11_record 1 "R11-C8 tools/stradapp s/// & tr///" "$(cat "$aud_dir/pp_c8.log" "$aud_dir/r11_pp_c8_bin.diag" 2>/dev/null)"
fi
