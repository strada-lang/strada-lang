#!/bin/bash
# t_audit_r17.sh - Cycle 17 regression tests across compiler, runtime, stdlib/C-exts, tools, Eval/VM/JIT

r17_dir="$BUILD_DIR/audit_r17"
mkdir -p "$r17_dir"

r17_record() {
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

# 1. Compiler & Runtime core suite (-O0 by default, plus -O2 under --default)
for opt in ${STRADA_TEST_AUDIT_OPTS:--O0}; do
    out=$("$PROJECT_DIR/strada" "$opt" "$SCRIPT_DIR/audit_r17/compiler_runtime.strada" -o "$r17_dir/comp_${opt}" 2>&1 \
        && "$r17_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R17_COMPILER_RUNTIME_OK"
    r17_record $? "Cycle 17 Compiler & Runtime suite ($opt)" "rc=$rc out=$out"
done

# 2. B8: Lexer iterative comment / block-comment / POD skipping (60k runs without stack overflow)
{
    perl -e 'print "# line $_\n" for 1..60000'
    perl -e 'print "/* blk $_ */\n" for 1..20000'
    echo "=pod"
    perl -e 'print "pod line $_\n" for 1..5000'
    echo "=cut"
    echo 'func main() int { say("B8_LEX_OK"); return 0; }'
} > "$r17_dir/b8_big_comments.strada"
b8_comp_out=$("$PROJECT_DIR/strada" -O0 "$r17_dir/b8_big_comments.strada" -o "$r17_dir/b8_comp" 2>&1 \
    && "$r17_dir/b8_comp" 2>&1)
b8_comp_rc=$?
b8_vm_out=$("$PROJECT_DIR/interpreter/strada-interp" "$r17_dir/b8_big_comments.strada" 2>&1)
b8_vm_rc=$?
[ "$b8_comp_rc" -eq 0 ] && [ "$b8_comp_out" = "B8_LEX_OK" ] && [ "$b8_vm_rc" -eq 0 ] && [ "$b8_vm_out" = "B8_LEX_OK" ]
r17_record $? "B8 Lexer iterative 80k comment/POD skip (compiled + VM)" "comp_rc=$b8_comp_rc('$b8_comp_out') vm_rc=$b8_vm_rc('$b8_vm_out')"

# 3. B11 & B14: VM compiler error rejections for illegal gotos & undeclared destructure target
cat > "$r17_dir/b11_into_if.strada" << 'EOF'
func main() int {
    goto INSIDE_IF;
    if (1) {
    INSIDE_IF:
        return 1;
    }
    return 0;
}
EOF
b11_1_out=$("$PROJECT_DIR/interpreter/strada-interp" "$r17_dir/b11_into_if.strada" 2>&1)
b11_1_rc=$?

cat > "$r17_dir/b11_cross_branch.strada" << 'EOF'
func main() int {
    if (1) {
        goto OTHER_BRANCH;
    } else {
    OTHER_BRANCH:
        return 1;
    }
    return 0;
}
EOF
b11_2_out=$("$PROJECT_DIR/interpreter/strada-interp" "$r17_dir/b11_cross_branch.strada" 2>&1)
b11_2_rc=$?

cat > "$r17_dir/b11_into_try.strada" << 'EOF'
func main() int {
    goto INSIDE_TRY;
    try {
    INSIDE_TRY:
        return 1;
    } catch ($e) {
    }
    return 0;
}
EOF
b11_3_out=$("$PROJECT_DIR/interpreter/strada-interp" "$r17_dir/b11_into_try.strada" 2>&1)
b11_3_rc=$?

[ "$b11_1_rc" -ne 0 ] && [ "$b11_2_rc" -ne 0 ] && [ "$b11_3_rc" -ne 0 ] && \
    printf '%s\n%s\n%s\n' "$b11_1_out" "$b11_2_out" "$b11_3_out" | grep -q "goto"
r17_record $? "B11 VM rejects goto into inner scope, across scopes, and into try" \
    "rc=($b11_1_rc,$b11_2_rc,$b11_3_rc)"

cat > "$r17_dir/b14_undecl_destr.strada" << 'EOF'
func main() int {
    my int $a = 0;
    ($a, $undecl_r17_var) = (1, 2);
    return 0;
}
EOF
b14_out=$("$PROJECT_DIR/interpreter/strada-interp" "$r17_dir/b14_undecl_destr.strada" 2>&1)
b14_rc=$?
[ "$b14_rc" -ne 0 ] && printf '%s\n' "$b14_out" | grep -q "undeclared variable"
r17_record $? "B14 VM rejects undeclared variable in destructuring assignment" "rc=$b14_rc out=$b14_out"

# 4. B15: strada-interp -e multiline heredoc + REPL .quit with regex containing quotes/semi
b15_e1_out=$("$PROJECT_DIR/interpreter/strada-interp" -e $'my str $h = <<\'E17\';\nline one\nline two;\nE17\nsay($h)' 2>&1)
b15_e1_rc=$?
b15_e2_out=$("$PROJECT_DIR/interpreter/strada-interp" -e $'say(<<\'E18\')\nfirst\nE18' 2>&1)
b15_e2_rc=$?
repl17_out=$(printf 'my str $s = "a;b"; ($s =~ /[\x27";]/) ? 1 : 0\n.quit\n' | "$PROJECT_DIR/interpreter/strada-interp" 2>&1)
repl17_rc=$?
[ "$b15_e1_rc" -eq 0 ] && [ "$b15_e2_rc" -eq 0 ] && \
    printf '%s\n' "$b15_e1_out" | grep -qx "line one" && printf '%s\n' "$b15_e1_out" | grep -qx "line two;" && \
    [ "$b15_e2_out" = "first" ] && \
    [ "$repl17_rc" -eq 0 ] && printf '%s\n' "$repl17_out" | grep -q "^1$"
r17_record $? "B15 strada-interp -e multiline heredoc & REPL regex auto-semi" "e1_rc=$b15_e1_rc('$b15_e1_out') e2_rc=$b15_e2_rc('$b15_e2_out') repl_rc=$repl17_rc"

# 5. B17: Driver --static-lib containing Eval linked with both non-Eval host & Eval host
b17_out=$(
    set -e
    cat > "$r17_dir/eval_wrap17.strada" << 'EOF'
package EvalWrap17;
use Eval;
func run_ev(str $c) scalar { return Strada::Interpreter::eval($c); }
EOF
    "$PROJECT_DIR/strada" --static-lib -L "$PROJECT_DIR/lib" "$r17_dir/eval_wrap17.strada" -o "$r17_dir/libew17.a" >/dev/null 2>&1

    # Host A: does not use Eval itself, links libew17.a (tests archive order & archive -rdynamic detection)
    cat > "$r17_dir/host_no_ev.strada" << 'EOF'
func main() int {
    my scalar $r = EvalWrap17::run_ev("19 + 23");
    say("HOST_A=" . $r);
    return 0;
}
EOF
    "$PROJECT_DIR/strada" -O0 "$r17_dir/host_no_ev.strada" "$r17_dir/libew17.a" -o "$r17_dir/host_no_ev" >/dev/null 2>&1
    "$r17_dir/host_no_ev" | grep -qx "HOST_A=42"

    # Host B: ALSO uses Eval, links libew17.a (must not fail with duplicate vm.o/vm_compiler.o symbols)
    cat > "$r17_dir/host_with_ev.strada" << 'EOF'
use Eval;
func main() int {
    my scalar $a = Strada::Interpreter::eval("10 + 5");
    my scalar $b = EvalWrap17::run_ev("20 + 7");
    say("HOST_B=" . ($a + $b));
    return 0;
}
EOF
    "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r17_dir/host_with_ev.strada" "$r17_dir/libew17.a" -o "$r17_dir/host_with_ev" >/dev/null 2>&1
    "$r17_dir/host_with_ev" | grep -qx "HOST_B=42"
    echo "B17_STATIC_EVAL_OK"
) 2>&1
b17_rc=$?
[ "$b17_rc" -eq 0 ] && printf '%s\n' "$b17_out" | grep -qx "B17_STATIC_EVAL_OK"
r17_record $? "B17 Driver --static-lib + Eval links cleanly with & without host Eval" "rc=$b17_rc out=$b17_out"

# 6. B18: lib/perl5 SvPV_nomg calls FETCH only once on tied Perl scalars (if ExtUtils::Embed available)
p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts 2>/dev/null || true)
if [ -n "$p5_flags" ]; then
    cat > "$r17_dir/r17_perl5.strada" << EOF
use lib "$PROJECT_DIR/lib";
use perl5;
func main() int {
    if (perl5::init() != 1) { return 1; }
    perl5::run(q{
        { package R17P5Cnt;
          our \$fetches = 0;
          sub TIESCALAR { my \$v = \$_[1]; bless \\\$v, \$_[0]; }
          sub FETCH { \$fetches++; my \$s = shift; return "V:" . \$\$s; }
          sub STORE { my (\$s, \$v) = @_; \$\$s = \$v; }
        }
        our \$t_var; tie \$t_var, "R17P5Cnt", "g";
        our \$t_ev;  tie \$t_ev,  "R17P5Cnt", "e";
        sub r17_ret_ts { my \$v; tie \$v, "R17P5Cnt", "c"; return \$v; }
    });
    my str \$gv = perl5::get_var("t_var");
    my int \$c1 = perl5::get_var("R17P5Cnt::fetches") + 0;
    perl5::eval("\$R17P5Cnt::fetches = 0;");
    my str \$es = perl5::eval("\$t_ev");
    my int \$c2 = perl5::get_var("R17P5Cnt::fetches") + 0;
    perl5::eval("\$R17P5Cnt::fetches = 0;");
    my str \$cs = perl5::call("r17_ret_ts", []);
    my int \$c3 = perl5::get_var("R17P5Cnt::fetches") + 0;
    perl5::shutdown();
    if (\$gv ne "V:g" || \$c1 != 1 || \$es ne "V:e" || \$c2 != 1 || \$cs ne "V:c" || \$c3 != 1) {
        say("BAD gv=\$gv c1=\$c1 es=\$es c2=\$c2 cs=\$cs c3=\$c3");
        return 2;
    }
    say("R17_PERL5_STRADA_OK");
    return 0;
}
EOF
    p5_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r17_dir/r17_perl5.strada" $p5_flags -o "$r17_dir/r17_perl5" 2>&1 \
        && timeout 20 "$r17_dir/r17_perl5" 2>&1)
    p5_rc=$?
    [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "R17_PERL5_STRADA_OK"
    r17_record $? "B18 lib/perl5.strada SvPV_nomg invokes FETCH once on tied scalars" "rc=$p5_rc out=$p5_out"

    cat > "$r17_dir/r17_perl5_c.c" << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strada_runtime.h"
extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_run_raw(const char *code);
extern char* strada_perl5_eval_raw(const char *code);
extern char* strada_perl5_get_scalar_raw(const char *name);
extern char* strada_perl5_call_str_raw(const char *sub_name, const char *arg);
extern StradaValue* strada_perl5_call_array(StradaValue *sub_sv, StradaValue *args_sv);
int main(void) {
    assert(strada_perl5_init() == 1);
    strada_perl5_run_raw(
        "{ package R17CP5; our $cnt = 0; "
        "  sub TIESCALAR { my $v = $_[1]; bless \\$v, $_[0]; } "
        "  sub FETCH { $cnt++; my $s = shift; return 'X:' . $$s; } "
        "  sub STORE { my ($s, $v) = @_; $$s = $v; } } "
        "our ($tv, $te, $tc); tie $tv, 'R17CP5', 'v'; tie $te, 'R17CP5', 'e'; "
        "sub ret_c { my $x; tie $x, 'R17CP5', 's'; return $x; } "
        "sub ret_a { my $x; tie $x, 'R17CP5', 'a'; return ($x); }"
    );
    char *s1 = strada_perl5_get_scalar_raw("tv");
    char *f1 = strada_perl5_get_scalar_raw("R17CP5::cnt");
    assert(strcmp(s1, "X:v") == 0 && strcmp(f1, "1") == 0);
    free(s1); free(f1);

    strada_perl5_run_raw("$R17CP5::cnt = 0;");
    char *s2 = strada_perl5_eval_raw("$te");
    char *f2 = strada_perl5_get_scalar_raw("R17CP5::cnt");
    assert(strcmp(s2, "X:e") == 0 && strcmp(f2, "1") == 0);
    free(s2); free(f2);

    strada_perl5_run_raw("$R17CP5::cnt = 0;");
    char *s3 = strada_perl5_call_str_raw("ret_c", NULL);
    char *f3 = strada_perl5_get_scalar_raw("R17CP5::cnt");
    assert(strcmp(s3, "X:s") == 0 && strcmp(f3, "1") == 0);
    free(s3); free(f3);

    strada_perl5_run_raw("$R17CP5::cnt = 0;");
    StradaValue *sub = strada_new_str("ret_a");
    StradaValue *arr = strada_perl5_call_array(sub, NULL);
    StradaValue *el = strada_array_get(arr->value.av, 0);
    char *f4 = strada_perl5_get_scalar_raw("R17CP5::cnt");
    assert(strcmp(el->value.pv, "X:a") == 0 && strcmp(f4, "1") == 0);
    free(f4);
    strada_decref(sub); strada_decref(arr);
    strada_perl5_shutdown();
    printf("R17_PERL5_C_OK\n");
    return 0;
}
EOF
    p5c_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r17_dir/r17_perl5_c.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" \
        "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS $p5_flags -o "$r17_dir/r17_perl5_c" 2>&1 \
        && timeout 20 "$r17_dir/r17_perl5_c" 2>&1)
    p5c_rc=$?
    [ "$p5c_rc" -eq 0 ] && printf '%s\n' "$p5c_out" | grep -qx "R17_PERL5_C_OK"
    r17_record $? "B18 strada_perl5.c SvPV_nomg invokes FETCH once across raw/array helpers" "rc=$p5c_rc out=$p5c_out"
fi

# 7. Eval / JIT / Worker VM / YAML / JSON::PS suite
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r17/eval_jit.strada" -o "$r17_dir/eval_jit" 2>&1 \
    && timeout 20 "$r17_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R17_EVAL_JIT_OK"
r17_record $? "Cycle 17 Eval / JIT / Worker VM / YAML / JSON::PS suite" "rc=$ej_rc out=$ej_out"

# 8. Compiled vs bytecode VM 2-way parity (test_audit_r17_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r17_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r17_vm.expected")
p_comp_err=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r17_dir/par_comp" 2>&1)
p_comp_rc=$?
if [ "$p_comp_rc" -eq 0 ]; then
    p_comp=$("$r17_dir/par_comp" 2>&1)
else
    p_comp="$p_comp_err"
fi
p_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp_rc" -eq 0 ] && [ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r17_record $? "Cycle 17 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"
