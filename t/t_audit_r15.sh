#!/bin/bash
# t_audit_r15.sh - Cycle 15 regression tests across compiler, runtime, stdlib/C-exts, Eval/VM/JIT

r15_dir="$BUILD_DIR/audit_r15"
mkdir -p "$r15_dir"

r15_record() {
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

# 1. Compiler & Runtime core suite at -O0 and -O2
for opt in -O0 -O2; do
    out=$("$PROJECT_DIR/strada" "$opt" "$SCRIPT_DIR/audit_r15/compiler_runtime.strada" -o "$r15_dir/comp_${opt}" 2>&1 \
        && "$r15_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R15_COMPILER_RUNTIME_OK"
    r15_record $? "Cycle 15 Compiler & Runtime suite ($opt)" "rc=$rc out=$out"
done

# 2. R15-C2: stradapp empty regex // before <<'EOF' heredoc containing #endif & #if
cat > "$r15_dir/pp_regex_heredoc.strada" << 'EOF'
func main() int {
    my array @a = split(//, <<'R15_PP_END');
#endif
#if 0
ab
R15_PP_END
    my str $s = join(":", @a);
    if (size(@a) < 12) { return 1; }
    say("R15_C2_PP_OK");
    return 0;
}
EOF
pp_out=$("$PROJECT_DIR/strada" -O0 "$r15_dir/pp_regex_heredoc.strada" -o "$r15_dir/pp_regex_heredoc" 2>&1 \
    && "$r15_dir/pp_regex_heredoc" 2>&1)
pp_rc=$?
[ "$pp_rc" -eq 0 ] && printf '%s\n' "$pp_out" | grep -qx "R15_C2_PP_OK"
r15_record $? "R15-C2 stradapp empty regex // before <<'EOF' heredoc with directives" "rc=$pp_rc out=$pp_out"

# 3. R15-RT4 (strada_str_len), R15-RT8 (circular STRADA_REF strada_av_unwrap_tied), R15-RT9 (io_poller_main timeout >= 2^31)
cat > "$r15_dir/r15_rt_c.c" << 'EOF'
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include "strada_runtime.h"

static StradaValue* r15_ov_bin(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_new_str_len("a\0b", 3);
}

int main(void) {
    strada_oop_init();
    strada_overload_register("R15CStrLenOv", "\"\"", r15_ov_bin);

    /* R15-RT4: strada_str_len preserves embedded NUL length */
    StradaValue *s = strada_new_str_len("x\0yz", 4);
    assert(strada_str_len(s) == 4);
    StradaValue *ov = strada_bless(strada_new_hash(), "R15CStrLenOv");
    assert(strada_str_len(ov) == 3);
    strada_decref(s);
    strada_decref(ov);

    /* R15-RT8: strada_av_unwrap_tied / strada_hv_unwrap_meta_sv throws on direct self-cycle */
    StradaValue *tmp_u = strada_new_undef();
    StradaValue *cyc = strada_new_ref(tmp_u, '@');
    strada_decref(tmp_u);
    strada_decref(cyc->value.rv);
    cyc->value.rv = cyc;
    int caught = 0;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        strada_av_unwrap_tied(cyc);
    } else {
        caught = 1;
    }
    STRADA_TRY_POP();
    assert(caught == 1);
    cyc->value.rv = NULL;
    strada_decref(cyc);

    /* R15-RT9: io_poller_main must not overflow int timeout for >= 2^31 ms */
    int p1[2], p2[2];
    assert(pipe(p1) == 0 && pipe(p2) == 0);
    StradaValue *mr = strada_new_str("r");
    StradaValue *f1 = strada_io_wait_async(STRADA_MAKE_TAGGED_INT(p1[0]), mr, STRADA_MAKE_TAGGED_INT(50));
    StradaValue *f2 = strada_io_wait_async(STRADA_MAKE_TAGGED_INT(p2[0]), mr, STRADA_MAKE_TAGGED_INT(2147483648LL));
    StradaValue *f3 = strada_io_wait_async(STRADA_MAKE_TAGGED_INT(p2[0]), mr, STRADA_MAKE_TAGGED_INT(4294967296LL));
    StradaValue *r1 = strada_future_await(f1);
    assert(r1 && r1->type == STRADA_STR && strcmp(r1->value.pv, "timeout") == 0);
    assert(write(p2[1], "Z", 1) == 1);
    StradaValue *r2 = strada_future_await(f2);
    StradaValue *r3 = strada_future_await(f3);
    assert(r2 && r2->type == STRADA_STR && strcmp(r2->value.pv, "r") == 0);
    assert(r3 && r3->type == STRADA_STR && strcmp(r3->value.pv, "r") == 0);
    strada_decref(r1); strada_decref(r2); strada_decref(r3);
    strada_decref(f1); strada_decref(f2); strada_decref(f3);
    strada_decref(mr);
    close(p1[0]); close(p1[1]);
    close(p2[0]); close(p2[1]);

    puts("R15_RT_C_OK");
    return 0;
}
EOF
rtc_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r15_dir/r15_rt_c.c" "$PROJECT_DIR/runtime/strada_runtime.o" \
    -lm -ldl -lpthread $EXTRA_LDFLAGS -o "$r15_dir/r15_rt_c" 2>&1 \
    && timeout 5 "$r15_dir/r15_rt_c" 2>&1)
rtc_rc=$?
[ "$rtc_rc" -eq 0 ] && printf '%s\n' "$rtc_out" | grep -qx "R15_RT_C_OK"
r15_record $? "R15-RT4/RT8/RT9 C runtime checks (strada_str_len, cycle throw, io_poller timeout)" "rc=$rtc_rc out=$rtc_out"

# 4. Stdlib suite (R15-L1, L2, L4, L5)
std_libs="-lz"
if [ -n "${STRADA_DBI_LIBS:-}" ]; then
    std_libs="$std_libs $STRADA_DBI_LIBS"
else
    [ "${STRADA_HAVE_SQLITE:-0}" = "1" ] && std_libs="$std_libs -lsqlite3"
    [ "${STRADA_HAVE_MYSQL:-0}" = "1" ] && std_libs="$std_libs ${STRADA_MYSQL_LIBS:--lmysqlclient}"
fi
ssl_args=""
if [ "${STRADA_HAVE_SSL:-0}" = "1" ] && command -v openssl >/dev/null 2>&1; then
    std_libs="$std_libs ${STRADA_SSL_LIBS:--lssl -lcrypto}"
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$r15_dir/key.pem" -out "$r15_dir/cert.pem" \
        -days 1 -subj "/CN=127.0.0.1" >/dev/null 2>&1
    ssl_port=$((28000 + ($$ % 2000)))
    ssl_args="$r15_dir/cert.pem $r15_dir/key.pem $ssl_port"
fi
std_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r15/stdlib_tools.strada" $std_libs -o "$r15_dir/stdlib" 2>&1 \
    && timeout 15 "$r15_dir/stdlib" $ssl_args 2>&1)
std_rc=$?
[ "$std_rc" -eq 0 ] && printf '%s\n' "$std_out" | grep -qx "AUDIT_R15_STDLIB_TOOLS_OK"
r15_record $? "Cycle 15 Stdlib suite (JSON overloads/canonical-mut, DBI tied single-FETCH/NULL, SSL port/64-bit read)" "rc=$std_rc out=$std_out"

# 5. R15-L3: perl5 tied hash/array must not call scalar 0-arg FETCH (both C API and Strada perl5::call)
if command -v perl >/dev/null 2>&1 && perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)
    cat > "$r15_dir/r15_perl5_c.c" << 'EOF'
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "strada_runtime.h"

extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_run_raw(const char *code);
extern StradaValue* strada_perl5_call_sv(StradaValue *sub_sv, StradaValue *args_sv);
extern void strada_perl5_set_scalar(StradaValue *name_sv, StradaValue *val_sv);
extern StradaValue* strada_perl5_get_scalar_sv(StradaValue *name_sv);

static int zero_arg_fetch_calls = 0;

static StradaValue* th_new(StradaValue *cls, StradaValue *args) {
    (void)cls; (void)args;
    return strada_bless(strada_new_hash(), "R15TieHash");
}
static StradaValue* th_fetch(StradaValue *self, StradaValue *args) {
    (void)self;
    if (!args || !args->value.av || args->value.av->size == 0) {
        zero_arg_fetch_calls++;
    }
    return strada_new_str("BAD_SCALAR_FETCH");
}
static StradaValue* ta_new(StradaValue *cls, StradaValue *args) {
    (void)cls; (void)args;
    return strada_bless(strada_new_hash(), "R15TieArray");
}
static StradaValue* ta_fetch(StradaValue *self, StradaValue *args) {
    (void)self;
    if (!args || !args->value.av || args->value.av->size == 0) {
        zero_arg_fetch_calls++;
    }
    return strada_new_str("BAD_ARR_FETCH");
}
static StradaValue* ta_fetchsize(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_new_int(0);
}

int main(void) {
    strada_oop_init();
    strada_method_register("R15TieHash", "TIEHASH", th_new);
    strada_method_register("R15TieHash", "FETCH", th_fetch);
    strada_method_register("R15TieArray", "TIEARRAY", ta_new);
    strada_method_register("R15TieArray", "FETCH", ta_fetch);
    strada_method_register("R15TieArray", "FETCHSIZE", ta_fetchsize);

    assert(strada_perl5_init() == 1);
    strada_perl5_run_raw("sub r15_p5_echo { return $_[0]; }");

    StradaValue *th = strada_new_hash();
    StradaValue *th_ref = strada_new_ref(th, '%');
    strada_decref(strada_tie_hash(th_ref, "R15TieHash", 0));

    StradaValue *ta = strada_new_array();
    StradaValue *ta_ref = strada_new_ref(ta, '@');
    strada_decref(strada_tie_array(ta_ref, "R15TieArray", 0));

    StradaValue *sub = strada_new_str("r15_p5_echo");
    StradaValue *r1 = strada_perl5_call_sv(sub, th);
    assert(r1 && r1->type == STRADA_STR && strncmp(r1->value.pv, "HASH(", 5) == 0);
    StradaValue *args1 = strada_new_array();
    strada_array_push(args1->value.av, ta);
    StradaValue *r2 = strada_perl5_call_sv(sub, args1);
    assert(r2 && r2->type == STRADA_STR && strcmp(r2->value.pv, "BAD_ARR_FETCH") != 0);
    StradaValue *r3 = strada_perl5_call_sv(th, sub);
    assert(r3 && r3->type == STRADA_STR && strstr(r3->value.pv, "BAD_SCALAR_FETCH") == NULL);

    StradaValue *vname = strada_new_str("r15_p5_var");
    strada_perl5_set_scalar(vname, th);
    StradaValue *g1 = strada_perl5_get_scalar_sv(vname);
    assert(g1 && g1->type == STRADA_STR && strncmp(g1->value.pv, "HASH(", 5) == 0);

    assert(zero_arg_fetch_calls == 0);
    strada_decref(r1); strada_decref(r2); strada_decref(r3); strada_decref(g1);
    strada_decref(args1); strada_decref(sub); strada_decref(vname);
    strada_decref(th_ref); strada_decref(th);
    strada_decref(ta_ref); strada_decref(ta);
    strada_perl5_shutdown();
    puts("R15_PERL5_C_OK");
    return 0;
}
EOF
    p5_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r15_dir/r15_perl5_c.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" \
        "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS $p5_flags -o "$r15_dir/r15_perl5_c" 2>&1 \
        && timeout 20 "$r15_dir/r15_perl5_c" 2>&1)
    p5_rc=$?
    [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "R15_PERL5_C_OK"
    r15_record $? "R15-L3 perl5 C API tied hash/array skips scalar FETCH" "rc=$p5_rc out=$p5_out"

    cat > "$r15_dir/r15_perl5_strada.strada" << EOF
use lib "$PROJECT_DIR/lib";
use perl5;
package R15P5StrTieHash;
func TIEHASH(str \$cls) scalar { return bless({"d" => {}}, \$cls); }
func FETCH(scalar \$s, str \$k) scalar { die("ZERO_ARG_FETCH"); }
func STORE(scalar \$s, str \$k, scalar \$v) void { \$s->{"d"}->{\$k} = \$v; }
package main;
func main() int {
    if (perl5::init() != 1) { return 1; }
    perl5::eval("sub r15_s_echo { return \$_[0]; }");
    my hash %h = ();
    tie(%h, "R15P5StrTieHash");
    my str \$r = perl5::call("r15_s_echo", [\%h]);
    my str \$r2 = perl5::call("r15_s_echo", \%h);
    untie(%h);
    perl5::shutdown();
    if (!(\$r =~ /^ARRAY\(/)) { return 2; }
    if (!(\$r2 =~ /^HASH\(/)) { return 3; }
    say("R15_PERL5_STRADA_OK");
    return 0;
}
EOF
    p5s_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r15_dir/r15_perl5_strada.strada" $p5_flags -o "$r15_dir/r15_perl5_strada" 2>&1 \
        && timeout 20 "$r15_dir/r15_perl5_strada" 2>&1)
    p5s_rc=$?
    [ "$p5s_rc" -eq 0 ] && printf '%s\n' "$p5s_out" | grep -qx "R15_PERL5_STRADA_OK"
    r15_record $? "R15-L3 lib/perl5.strada perl5::call tied hash skips scalar FETCH" "rc=$p5s_rc out=$p5s_out"
fi

# 6. Eval, eval_string_vm (R15-VM7, VM8, VM9), and Strada::JIT
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r15/eval_jit.strada" -o "$r15_dir/eval_jit" 2>&1 \
    && timeout 20 "$r15_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R15_EVAL_JIT_OK"
r15_record $? "Cycle 15 Eval / eval_string_vm / Strada::JIT suite (R15-VM1..9)" "rc=$ej_rc out=$ej_out"

# 7. Compiled vs bytecode VM 2-way parity (test_audit_r15_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r15_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r15_vm.expected")
p_comp=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r15_dir/par_comp" 2>&1 && "$r15_dir/par_comp" 2>&1)
p_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r15_record $? "Cycle 15 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"

# 8. R15-VM6 shutdown DESTROY without explicit gc_collect
cat > "$r15_dir/shut_dtor.strada" << 'EOF'
package R15ShutDtor;
func DESTROY(scalar $self) void {
    say("SHUT_DESTROY_RAN");
}
package main;
func main() int {
    our scalar $g_hold = bless({}, "R15ShutDtor");
    return 0;
}
EOF
sh_c=$("$PROJECT_DIR/strada" -O0 "$r15_dir/shut_dtor.strada" -o "$r15_dir/shut_dtor" 2>&1 && "$r15_dir/shut_dtor" 2>&1)
sh_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$r15_dir/shut_dtor.strada" 2>&1)
[ "$sh_c" = "SHUT_DESTROY_RAN" ] && [ "$sh_vm" = "SHUT_DESTROY_RAN" ]
r15_record $? "R15-VM6 shutdown DESTROY parity (compiled / VM)" "comp='$sh_c' vm='$sh_vm'"
