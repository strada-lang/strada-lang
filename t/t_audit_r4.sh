#!/bin/bash
# t_audit_r4.sh — Cycle 4 Audit regression suite

aud_dir="$BUILD_DIR/audit_r4"
aud_src="$SCRIPT_DIR/audit_r4"
mkdir -p "$aud_dir"

aud_r4_record() {
    local ok="$1"
    local name="$2"
    local diag="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$ok" -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "$name"
    else
        FAILED=$((FAILED + 1))
        log_fail "$name" "$diag"
    fi
}

aud_r4_fast_compile() {
    local src="$1"
    local name="$2"
    (
        cd "$PROJECT_DIR" &&
        BUILD_DIR="$aud_dir" STRADA_TEST_TCC=1 compile_strada "$src" "$name"
    )
}

(
    rc=0
    aud_r4_fast_compile "$aud_src/runtime.strada" "r4_runtime" &&
    timeout 25 "$aud_dir/r4_runtime" > "$aud_dir/runtime.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/runtime.rc"
) &
pid_rt=$!

(
    rc=0
    aud_r4_fast_compile "$aud_src/compiler.strada" "r4_compiler" &&
    timeout 25 "$aud_dir/r4_compiler" > "$aud_dir/compiler.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/compiler.rc"
) &
pid_comp=$!

wait "$pid_rt" "$pid_comp"

rt_rc=$(cat "$aud_dir/runtime.rc" 2>/dev/null || echo 1)
rt_out=$(cat "$aud_dir/runtime.out" 2>/dev/null || true)
if [ "$rt_rc" -eq 0 ] && echo "$rt_out" | grep -q "R4_RUNTIME_OK"; then
    aud_r4_record 0 "runtime (C4): regex subst captures/%+, UTF-8 %*v/rindex/split, concat unwind guards" ""
else
    aud_r4_record 1 "runtime (C4)" "rc=$rt_rc out=$rt_out"
fi

comp_rc=$(cat "$aud_dir/compiler.rc" 2>/dev/null || echo 1)
comp_out=$(cat "$aud_dir/compiler.out" 2>/dev/null || true)
if [ "$comp_rc" -eq 0 ] && echo "$comp_out" | grep -q "R4_COMPILER_OK"; then
    aud_r4_record 0 "compiler (C4): inc/dec expr, free() undef, pkg global init, </> diamond, #if0 skip" ""
else
    aud_r4_record 1 "compiler (C4)" "rc=$comp_rc out=$comp_out"
fi

# CC-07: -D NAME=VAL CLI defines preserving STRADA_DEFINES
cat > "$aud_dir/cli_def.strada" <<'EOF'
func main() int {
#if MY_NUM == 42 && ENV_DEF == 9
    say("CLI_DEF_OK");
    return 0;
#else
    say("CLI_DEF_FAIL");
    return 1;
#endif
}
EOF
def_rc=1
def_out=""
if STRADA_DEFINES="ENV_DEF=9" "$PROJECT_DIR/strada" -O0 -D MY_NUM=42 "$aud_dir/cli_def.strada" -o "$aud_dir/cli_def" > "$aud_dir/cli_def.build.log" 2>&1; then
    def_out=$(timeout 10 "$aud_dir/cli_def" 2>&1) && def_rc=0 || def_rc=$?
else
    def_out=$(cat "$aud_dir/cli_def.build.log" 2>/dev/null || true)
fi
if [ "$def_rc" -eq 0 ] && echo "$def_out" | grep -q "CLI_DEF_OK"; then
    aud_r4_record 0 "compiler (C4): -D NAME=VAL parses values and preserves STRADA_DEFINES" ""
else
    aud_r4_record 1 "compiler (C4): -D NAME=VAL" "rc=$def_rc out=$def_out"
fi

# CC-08: stradapp skips inactive __C__ blocks under #if 0
cat > "$aud_dir/pp_cblock.strada" <<'EOF'
#if 0
__C__ {
    this is invalid C syntax that would fail compilation! @@@
}
#endif
func main() int {
    say("PP_CBLOCK_OK");
    return 0;
}
EOF
pp_rc=1
pp_out=""
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_cblock.strada" -o "$aud_dir/pp_cblock.out.strada" > "$aud_dir/pp_cblock.log" 2>&1 \
    && "$PROJECT_DIR/strada" -O0 "$aud_dir/pp_cblock.out.strada" -o "$aud_dir/pp_cblock" >> "$aud_dir/pp_cblock.log" 2>&1; then
    pp_out=$(timeout 10 "$aud_dir/pp_cblock" 2>&1) && pp_rc=0 || pp_rc=$?
else
    pp_out=$(cat "$aud_dir/pp_cblock.log" 2>/dev/null || true)
fi
if [ "$pp_rc" -eq 0 ] && echo "$pp_out" | grep -q "PP_CBLOCK_OK"; then
    aud_r4_record 0 "stradapp (C4): skips inactive __C__ blocks inside #if 0" ""
else
    aud_r4_record 1 "stradapp (C4): #if 0 __C__ block" "rc=$pp_rc out=$pp_out"
fi

# CC-04 + SL-02: qw() imports in non-main package + DBI::quote_identifier UTF-8 preservation
cat > "$aud_dir/R4QwMod.strada" <<'EOF'
package R4QwMod;
func qw_add(int $a, int $b) int { return $a + $b; }
EOF
cat > "$aud_dir/pkg_qw_dbi.strada" <<EOF
use lib "$aud_dir";
use lib "$PROJECT_DIR/lib";
use DBI;

package R4Consumer;
use R4QwMod qw(qw_add);
func use_qw() int { return qw_add(19, 23); }

package main;
func main() int {
    if (R4Consumer::use_qw() != 42) { say("FAIL qw"); return 1; }
    my scalar \$dbh = DBI::connect("dbi:SQLite::memory:", "", "");
    my str \$id = "café";
    utf8::upgrade(\$id);
    my str \$qi = DBI::quote_identifier(\$dbh, \$id);
    DBI::disconnect(\$dbh);
    if (\$qi ne '"café"' || utf8::is_utf8(\$qi) != 1) {
        say("FAIL quote_identifier qi=" . \$qi . " utf8=" . utf8::is_utf8(\$qi));
        return 2;
    }
    say("QW_DBI_OK");
    return 0;
}
EOF
qwd_rc=1
qwd_out=""
if "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/pkg_qw_dbi.strada" -lsqlite3 -ldl -o "$aud_dir/pkg_qw_dbi" > "$aud_dir/qwd.log" 2>&1; then
    qwd_out=$(timeout 15 "$aud_dir/pkg_qw_dbi" 2>&1) && qwd_rc=0 || qwd_rc=$?
else
    qwd_out=$(tail -n 10 "$aud_dir/qwd.log" 2>/dev/null || true)
fi
if [ "$qwd_rc" -eq 0 ] && echo "$qwd_out" | grep -q "QW_DBI_OK"; then
    aud_r4_record 0 "compiler+stdlib (C4): package qw() imports + DBI::quote_identifier UTF-8 flag" ""
else
    aud_r4_record 1 "package qw() + DBI quote_identifier" "rc=$qwd_rc out=$qwd_out"
fi

# SL-03: lib/perl5 UTF-8 roundtrip (when ExtUtils::Embed is present)
if perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    cat > "$aud_dir/p5_utf8.strada" <<EOF
use lib "$PROJECT_DIR/lib";
use perl5;
func main() int {
    if (perl5::init() != 1) { say("FAIL p5 init"); return 1; }
    my str \$u = "café";
    utf8::upgrade(\$u);
    perl5::set_var("uv", \$u);
    my str \$got = perl5::get_var("uv");
    perl5::run('sub ulen { return length(\$_[0]); }');
    my str \$pl = perl5::call("ulen", \$u);
    perl5::shutdown();
    if (\$got ne "café") { say("FAIL get_var got=" . \$got); return 2; }
    if (length(\$pl) == 0) { say("FAIL call"); return 3; }
    say("P5_U8_OK");
    return 0;
}
EOF
    p5u_rc=1
    p5u_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)
    if make -C "$PROJECT_DIR/lib/perl5" > "$aud_dir/p5u.log" 2>&1 \
        && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/p5_utf8.strada" $p5u_flags -o "$aud_dir/p5_utf8" >> "$aud_dir/p5u.log" 2>&1; then
        p5u_out=$(timeout 15 "$aud_dir/p5_utf8" 2>&1) && p5u_rc=0 || p5u_rc=$?
    else
        p5u_out=$(tail -n 10 "$aud_dir/p5u.log" 2>/dev/null || true)
    fi
    make -C "$PROJECT_DIR/lib/perl5" clean >/dev/null 2>&1 || true
    if [ "$p5u_rc" -eq 0 ] && echo "$p5u_out" | grep -q "P5_U8_OK"; then
        aud_r4_record 0 "lib/perl5 (C4): STRADA_STR_BYTELEN marshaling preserves UTF-8 strings" ""
    else
        aud_r4_record 1 "lib/perl5 UTF-8 marshaling" "rc=$p5u_rc out=$p5u_out"
    fi
fi
