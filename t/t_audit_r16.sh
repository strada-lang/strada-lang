#!/bin/bash
# t_audit_r16.sh - Cycle 16 regression tests across compiler, runtime, stdlib/C-exts, tools, Eval/VM

r16_dir="$BUILD_DIR/audit_r16"
mkdir -p "$r16_dir"

r16_record() {
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
    out=$("$PROJECT_DIR/strada" "$opt" "$SCRIPT_DIR/audit_r16/compiler_runtime.strada" -o "$r16_dir/comp_${opt}" 2>&1 \
        && "$r16_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R16_COMPILER_RUNTIME_OK"
    r16_record $? "Cycle 16 Compiler & Runtime suite ($opt)" "rc=$rc out=$out"
done

# 2. S1-2: ext_destroy on unblessed cyclic objects and STRADA_CELL
cat > "$r16_dir/r16_ext_destroy.c" << 'EOF'
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "strada_runtime.h"

static int g_ext_freed = 0;
static void on_ext_free(StradaValue *obj, void *ud) {
    (void)obj;
    int *tag = (int *)ud;
    g_ext_freed += *tag;
    free(tag);
}

int main(void) {
    /* Case A: cyclic unblessed hash holding cpointer with ext_destroy */
    StradaValue *h1 = strada_new_hash();
    StradaValue *h2 = strada_new_hash();
    int *p1 = malloc(sizeof(int));
    *p1 = 10;
    StradaValue *cp = strada_cpointer_new(p1);
    StradaMetadata *mcp = strada_ensure_meta(cp);
    mcp->ext_destroy = on_ext_free;
    mcp->ext_destroy_ud = p1;
    strada_hash_set(h1->value.hv, "next", h2);
    strada_hash_set(h1->value.hv, "res", cp);
    strada_hash_set(h2->value.hv, "back", h1);
    strada_decref(cp);
    strada_decref(h2);
    strada_decref(h1);
    strada_gc_collect();
    assert(g_ext_freed == 10);

    /* Case B: STRADA_CELL holding ext_destroy in a cycle */
    StradaValue **cell = strada_cell_alloc();
    StradaValue *c = STRADA_CELL_SV_FROM_SLOT(cell);
    int *p2 = malloc(sizeof(int));
    *p2 = 7;
    StradaMetadata *mc = strada_ensure_meta(c);
    mc->ext_destroy = on_ext_free;
    mc->ext_destroy_ud = p2;
    StradaValue *arr = strada_new_array();
    strada_array_push(arr->value.av, c);
    *cell = arr;
    strada_incref(arr);
    strada_decref(arr);
    strada_decref(c);
    strada_gc_collect();
    assert(g_ext_freed == 17);

    printf("R16_EXT_DESTROY_OK\n");
    return 0;
}
EOF
ext_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r16_dir/r16_ext_destroy.c" "$PROJECT_DIR/runtime/strada_runtime.o" \
    -lm -ldl -lpthread $EXTRA_LDFLAGS -o "$r16_dir/r16_ext_destroy" 2>&1 \
    && timeout 20 "$r16_dir/r16_ext_destroy" 2>&1)
ext_rc=$?
[ "$ext_rc" -eq 0 ] && printf '%s\n' "$ext_out" | grep -qx "R16_EXT_DESTROY_OK"
r16_record $? "S1-2 cycle GC fires ext_destroy on unblessed & STRADA_CELL cycles" "rc=$ext_rc out=$ext_out"

# 3. S1-3: POSIX fallback (#ifndef HAVE_PCRE2) s///g match count + NUL-safe len2
cat > "$r16_dir/r16_posix_regex.c" << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strada_runtime.h"

int main(void) {
    char *out = NULL;
    size_t out_len = 0;
    int cnt = strada_regex_replace_capture_len2(
        "ab1ab2ab", 8, 0,
        "ab", 2,
        "X\0Y", 3,
        "", 1, &out, &out_len);
    assert(cnt == 3);
    assert(out_len == 11);
    assert(memcmp(out, "X\0Y1X\0Y2X\0Y", 11) == 0);
    free(out);

    char *out2 = NULL;
    int cnt2 = strada_regex_replace_capture("aaa", "a", "b", "", 1, &out2);
    assert(cnt2 == 3);
    assert(strcmp(out2, "bbb") == 0);
    free(out2);

    printf("R16_POSIX_REGEX_OK\n");
    return 0;
}
EOF
pox_out=$($GCC -O0 -UHAVE_PCRE2 -UPCRE2_STATIC -DSTRADA_NO_TLS -I"$PROJECT_DIR/runtime" \
    "$r16_dir/r16_posix_regex.c" "$PROJECT_DIR/runtime/strada_runtime.c" \
    -lm -ldl -lpthread -o "$r16_dir/r16_posix_regex" 2>&1 \
    && timeout 20 "$r16_dir/r16_posix_regex" 2>&1)
pox_rc=$?
[ "$pox_rc" -eq 0 ] && printf '%s\n' "$pox_out" | grep -qx "R16_POSIX_REGEX_OK"
r16_record $? "S1-3 POSIX regex fallback s///g count and NUL-safe replacement" "rc=$pox_rc out=$pox_out"

# 4. S2-5: binary // defined-or must not disable regex // scanning in Lexer or stradapp
cat > "$r16_dir/pp_dor_regex.strada" << 'EOF'
func main() int {
    my scalar $u = undef;
    my str $a = $u // "fallback";
    my array @parts = split(//, <<'R16_PP_HD');
#ifdef NO_SUCH_FLAG
#error_line
#endif
ok
R16_PP_HD
    my int $flag_on = 0;
#ifdef R16_PP_FLAG
    $flag_on = 1;
#endif
    my str $j = join(":", @parts);
    if ($a ne "fallback" || index($j, "#:e:n:d:i:f") < 0 || $flag_on != 1) {
        return 1;
    }
    say("S2_5_PP_OK");
    return 0;
}
EOF
s25_out=$("$PROJECT_DIR/strada" -O0 -D R16_PP_FLAG "$r16_dir/pp_dor_regex.strada" -o "$r16_dir/pp_dor_regex" 2>&1 \
    && "$r16_dir/pp_dor_regex" 2>&1)
s25_rc=$?
[ "$s25_rc" -eq 0 ] && printf '%s\n' "$s25_out" | grep -qx "S2_5_PP_OK"
r16_record $? "S2-5 Lexer/stradapp // defined-or vs empty regex // before heredoc & #ifdef" "rc=$s25_rc out=$s25_out"

# 5. S4-1 (--module-cache + Eval + -rdynamic), S4-2 (--static-lib imported .o), S4-3 (--shared split-shards + Eval)
s4_out=$(
    set -e
    mkdir -p "$r16_dir/altlib/Strada" "$r16_dir/mcache"
    cp "$PROJECT_DIR/lib/Eval.strada" "$r16_dir/altlib/"
    cp "$PROJECT_DIR/lib/Strada/Interpreter.strada" "$r16_dir/altlib/Strada/"

    cat > "$r16_dir/mcache_eval.strada" << 'EOF'
use Eval;
func main() int {
    say("MC_EV=" . Strada::Interpreter::eval("20 + 22"));
    return 0;
}
EOF
    # First warm cache cleanly, then link from cached Eval.strada.o with -rdynamic check
    STRADA_MODULE_CACHE_DIR="$r16_dir/mcache" "$PROJECT_DIR/strada" --module-cache -L "$r16_dir/altlib" \
        "$r16_dir/mcache_eval.strada" -o "$r16_dir/mc_ev1" >/dev/null 2>&1
    "$r16_dir/mc_ev1" | grep -qx "MC_EV=42"
    v_log=$(STRADA_MODULE_CACHE_DIR="$r16_dir/mcache" "$PROJECT_DIR/strada" -v --module-cache -L "$r16_dir/altlib" \
        "$r16_dir/mcache_eval.strada" -o "$r16_dir/mc_ev2" 2>&1)
    printf '%s\n' "$v_log" | grep -q -- "-rdynamic"
    "$r16_dir/mc_ev2" | grep -qx "MC_EV=42"

    # S4-2: --static-lib archives imported module .o files
    cat > "$r16_dir/ModS42.strada" << 'EOF'
package ModS42;
func hi(str $x) str { return "S42:" . $x; }
EOF
    "$PROJECT_DIR/strada" -M --object "$r16_dir/ModS42.strada" -o "$r16_dir/ModS42.o" >/dev/null 2>&1
    cat > "$r16_dir/wrap_s42.strada" << 'EOF'
use ModS42;
func call_s42(str $x) str { return ModS42::hi($x); }
EOF
    "$PROJECT_DIR/strada" --static-lib -L "$r16_dir" "$r16_dir/wrap_s42.strada" -o "$r16_dir/libwraps42.a" >/dev/null 2>&1
    ar t "$r16_dir/libwraps42.a" | grep -q "ModS42.o"

    # S4-3: --shared split-shard build (>=131072B) with Eval source
    {
        echo "use Eval;"
        perl -e 'print "func r16_pad_$_(int \$x) int { return \$x + $_; } # " . ("x" x 80) . "\n" for 1..1100'
        echo 'func sh_eval16(str $s) scalar { return Strada::Interpreter::eval($s); }'
    } > "$r16_dir/big_sh16.strada"
    "$PROJECT_DIR/strada" --shared -L "$r16_dir/altlib" "$r16_dir/big_sh16.strada" -o "$r16_dir/libbig_sh16.so" >/dev/null 2>&1
    echo "S4_ALL_OK"
) 2>&1
s4_rc=$?
[ "$s4_rc" -eq 0 ] && printf '%s\n' "$s4_out" | grep -qx "S4_ALL_OK"
r16_record $? "S4-1/S4-2/S4-3 driver --module-cache Eval, --static-lib .o, --shared split-shards Eval" "rc=$s4_rc out=$s4_out"

# 6. S4-4 / S4-5: lib/perl5 SvGETMAGIC + SvSETMAGIC on tied/magical scalars (if ExtUtils::Embed available)
p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts 2>/dev/null || true)
if [ -n "$p5_flags" ]; then
    cat > "$r16_dir/r16_perl5.strada" << EOF
use lib "$PROJECT_DIR/lib";
use perl5;
func main() int {
    if (perl5::init() != 1) { return 1; }
    perl5::run(q{
        { package R16P5Tie;
          sub TIESCALAR { my \$v = "init"; bless \\\$v, \$_[0]; }
          sub FETCH { my \$s = shift; return "F:" . \$\$s; }
          sub STORE { my (\$s, \$v) = @_; \$\$s = "S:" . \$v; }
        }
        our \$p5_ts;
        tie \$p5_ts, "R16P5Tie";
    });
    my str \$g1 = perl5::get_var("p5_ts");
    perl5::set_var("p5_ts", "hi");
    my str \$g2 = perl5::get_var("p5_ts");
    perl5::shutdown();
    if (\$g1 ne "F:init" || \$g2 ne "F:S:hi") { return 2; }
    say("R16_PERL5_STRADA_OK");
    return 0;
}
EOF
    p5_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r16_dir/r16_perl5.strada" $p5_flags -o "$r16_dir/r16_perl5" 2>&1 \
        && timeout 20 "$r16_dir/r16_perl5" 2>&1)
    p5_rc=$?
    [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "R16_PERL5_STRADA_OK"
    r16_record $? "S4-4/S4-5 lib/perl5.strada SvGETMAGIC and SvSETMAGIC on tied scalars" "rc=$p5_rc out=$p5_out"

    cat > "$r16_dir/r16_perl5_c.c" << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strada_runtime.h"
extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_run_raw(const char *code);
extern void strada_perl5_set_scalar(StradaValue *name_sv, StradaValue *val_sv);
extern StradaValue* strada_perl5_get_scalar_sv(StradaValue *name_sv);
extern void strada_perl5_set_scalar_raw(const char *name, const char *value);
extern char* strada_perl5_get_scalar_raw(const char *name);
extern StradaValue* strada_perl5_call_array(StradaValue *sub_sv, StradaValue *args_sv);
int main(void) {
    assert(strada_perl5_init() == 1);
    strada_perl5_run_raw(
        "{ package R16CP5T; sub TIESCALAR { my $v = \"a\"; bless \\$v, $_[0]; } "
        "  sub FETCH { my $s = shift; return \"F:\" . $$s; } "
        "  sub STORE { my ($s, $v) = @_; $$s = \"S:\" . $v; } } "
        "our $p5c_ts; tie $p5c_ts, \"R16CP5T\"; sub ret_p5c_ts { return ($p5c_ts); }"
    );
    StradaValue *vn = strada_new_str("p5c_ts");
    StradaValue *g1 = strada_perl5_get_scalar_sv(vn);
    assert(strcmp(g1->value.pv, "F:a") == 0);
    StradaValue *v2 = strada_new_str("b");
    strada_perl5_set_scalar(vn, v2);
    StradaValue *g2 = strada_perl5_get_scalar_sv(vn);
    assert(strcmp(g2->value.pv, "F:S:b") == 0);
    strada_perl5_set_scalar_raw("p5c_ts", "c");
    char *g3 = strada_perl5_get_scalar_raw("p5c_ts");
    assert(strcmp(g3, "F:S:c") == 0);
    StradaValue *sub = strada_new_str("ret_p5c_ts");
    StradaValue *arr = strada_perl5_call_array(sub, NULL);
    StradaValue *g4 = strada_array_get(arr->value.av, 0);
    assert(strcmp(g4->value.pv, "F:S:c") == 0);
    free(g3);
    strada_decref(vn); strada_decref(g1); strada_decref(v2);
    strada_decref(g2); strada_decref(sub); strada_decref(arr);
    strada_perl5_shutdown();
    printf("R16_PERL5_C_OK\n");
    return 0;
}
EOF
    p5c_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r16_dir/r16_perl5_c.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" \
        "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS $p5_flags -o "$r16_dir/r16_perl5_c" 2>&1 \
        && timeout 20 "$r16_dir/r16_perl5_c" 2>&1)
    p5c_rc=$?
    [ "$p5c_rc" -eq 0 ] && printf '%s\n' "$p5c_out" | grep -qx "R16_PERL5_C_OK"
    r16_record $? "S4-4/S4-5 strada_perl5.c SvGETMAGIC and SvSETMAGIC" "rc=$p5c_rc out=$p5c_out"
fi

# 7. S3-5 (unresolved goto in sort fails cleanly) & S3-11 (interp -e and REPL trailing # comment)
cat > "$r16_dir/bad_sort_goto.strada" << 'EOF'
func main() int {
    my array @a = sort { do { goto NO_SUCH_LABEL; 0; } } (2, 1);
    return 0;
}
EOF
bsg_out=$("$PROJECT_DIR/interpreter/strada-interp" "$r16_dir/bad_sort_goto.strada" 2>&1)
bsg_rc=$?
[ "$bsg_rc" -ne 0 ] && printf '%s\n' "$bsg_out" | grep -qE "unknown goto label|unresolved goto label"
r16_record $? "S3-5 VM sort comparator reports unresolved goto error" "rc=$bsg_rc out=$bsg_out"

e_out=$("$PROJECT_DIR/interpreter/strada-interp" -e 'say(1 + 2) # comment' 2>&1)
e_rc=$?
repl_out=$(printf '1 + 2 # comment\n3 + 4; # semi comment\n# only comment\n:quit\n' | "$PROJECT_DIR/interpreter/strada-interp" 2>&1)
repl_rc=$?
[ "$e_rc" -eq 0 ] && [ "$e_out" = "3" ] && [ "$repl_rc" -eq 0 ] && printf '%s\n' "$repl_out" | grep -q "^3$" && printf '%s\n' "$repl_out" | grep -q "^7$"
r16_record $? "S3-11 interp -e and REPL auto-semicolon ignores trailing # comments" "e='$e_out' repl='$repl_out'"

# 8. Eval / reentrant eval / thread-safety suite
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r16/eval_jit.strada" -o "$r16_dir/eval_jit" 2>&1 \
    && timeout 20 "$r16_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R16_EVAL_JIT_OK"
r16_record $? "Cycle 16 Eval / nested / threaded VM suite (S3-1..S3-11)" "rc=$ej_rc out=$ej_out"

# 9. Compiled vs bytecode VM 2-way parity (test_audit_r16_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r16_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r16_vm.expected")
p_comp_err=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r16_dir/par_comp" 2>&1)
p_comp_rc=$?
if [ "$p_comp_rc" -eq 0 ]; then
    p_comp=$("$r16_dir/par_comp" 2>&1)
else
    p_comp="$p_comp_err"
fi
p_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp_rc" -eq 0 ] && [ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r16_record $? "Cycle 16 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"
