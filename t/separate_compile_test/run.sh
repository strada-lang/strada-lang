#!/bin/bash
#
# Tests for separate compilation: -M flag, --object/--object-full, and
# `use Foo;` auto-detection of pre-compiled .o/.so artifacts.
#
# Usage: ./run.sh [-v]
#
# Exits non-zero on any failure. Each test prints PASS/FAIL.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
STRADAC="$REPO_DIR/stradac"

# This whole suite exercises `use Foo;` auto-detecting a precompiled sibling
# .o/.so, which is now opt-in (--use-artifacts / STRADA_USE_ARTIFACTS) since it
# runs the artifact's code at compile time. Opt in for the test.
export STRADA_USE_ARTIFACTS=1

if [ ! -x "$STRADA" ] || [ ! -x "$STRADAC" ]; then
    echo "Build strada first (run 'make' in $REPO_DIR)" >&2
    exit 2
fi

VERBOSE=0
if [ "${1:-}" = "-v" ]; then VERBOSE=1; fi

PASS=0
FAIL=0
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Pretty colours when on a tty
if [ -t 1 ]; then RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'; else RED=''; GREEN=''; NC=''; fi

pass() { PASS=$((PASS + 1)); printf "${GREEN}PASS${NC} %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "${RED}FAIL${NC} %s\n  %s\n" "$1" "${2:-}"; }

# --- Test fixtures ---------------------------------------------------------
mkdir -p "$WORK/proj"
cat > "$WORK/proj/Util.strada" <<'EOF'
package Util;
func doubled(int $x) int { return $x * 2; }
func tripled(int $x) int { return $x * 3; }
EOF

cat > "$WORK/proj/Big.strada" <<'EOF'
use lib ".";
use Util;
package Big;
func compute(int $x) int { return Util::doubled($x) + Util::tripled($x); }
EOF

cat > "$WORK/proj/app.strada" <<'EOF'
use lib ".";
use Big;
use Util;
func main() int {
    my int $r = Big::compute(5);
    if ($r != 25) { say("got " . $r); return 1; }
    say("OK");
    return 0;
}
EOF

cd "$WORK/proj"

# --- Test 1: -M FILE produces a sibling .o --------------------------------
rm -f Util.o
if "$STRADA" -M Util.strada >"$WORK/t1.log" 2>&1 && [ -f Util.o ]; then
    pass "-M FILE produces sibling .o"
else
    fail "-M FILE produces sibling .o" "$(cat "$WORK/t1.log")"
fi

# --- Test 2: -M produces module-only contents (no deps inlined) -----------
rm -f Big.o
if "$STRADA" -M Big.strada >"$WORK/t2.log" 2>&1 && [ -f Big.o ]; then
    # Big depends on Util but Util.o is fresh, so use auto-detects it.
    # Big.o should NOT contain a Util_doubled body (only references).
    if nm Big.o 2>/dev/null | grep -qE '^[0-9a-f]+ T Util_doubled$'; then
        fail "-M produces module-only contents" "Big.o has Util_doubled definition"
    else
        pass "-M produces module-only contents"
    fi
else
    fail "-M produces module-only contents" "$(cat "$WORK/t2.log")"
fi

# --- Test 3: --object-full bundles deps -----------------------------------
# Remove Util.o so use falls back to source inlining, then compare sizes.
rm -f Util.o Big.o Big_full.o
"$STRADA" --object -o Big_only.o Big.strada >"$WORK/t3a.log" 2>&1
"$STRADA" --object-full -o Big_full.o Big.strada >"$WORK/t3b.log" 2>&1
if [ -f Big_only.o ] && [ -f Big_full.o ]; then
    if nm Big_full.o 2>/dev/null | grep -qE '^[0-9a-f]+ T Util_doubled$' && \
       ! nm Big_only.o 2>/dev/null | grep -qE '^[0-9a-f]+ T Util_doubled$'; then
        pass "--object-full bundles deps; --object does not"
    else
        fail "--object-full bundles deps; --object does not" "symbol presence mismatch"
    fi
else
    fail "--object-full bundles deps; --object does not" "compilation failed"
fi

# --- Test 4: end-to-end: compile + run with auto-detect ------------------
rm -f Util.o Big.o app
"$STRADA" -M Util.strada >"$WORK/t4a.log" 2>&1
"$STRADA" -M Big.strada >"$WORK/t4b.log" 2>&1
if "$STRADA" -o app app.strada >"$WORK/t4c.log" 2>&1 && [ -x app ]; then
    out="$(./app)"
    if [ "$out" = "OK" ]; then
        pass "end-to-end separate compilation + auto-detect"
    else
        fail "end-to-end separate compilation + auto-detect" "output was: $out"
    fi
else
    fail "end-to-end separate compilation + auto-detect" "$(cat "$WORK/t4c.log")"
fi

# --- Test 5: stale .o is ignored -----------------------------------------
# Touch Util.strada so its mtime > Util.o, then a fresh app build should
# fall back to source inlining (no "import_object files: Util.o" line).
sleep 1
touch Util.strada
verbose_out="$("$STRADA" -v -o app app.strada 2>&1)"
if echo "$verbose_out" | grep -q 'import_object files:.*Util.o'; then
    fail "stale .o is ignored" "Util.o was used despite stale source"
else
    pass "stale .o is ignored (source re-inlined)"
fi

# --- Test 6: -M DIR recursive walk ---------------------------------------
mkdir -p "$WORK/tree/sub"
cat > "$WORK/tree/A.strada" <<'EOF'
package A; func a() str { return "a"; }
EOF
cat > "$WORK/tree/sub/B.strada" <<'EOF'
package B; func b() str { return "b"; }
EOF
if "$STRADA" -M "$WORK/tree" >"$WORK/t6.log" 2>&1 && \
   [ -f "$WORK/tree/A.o" ] && [ -f "$WORK/tree/sub/B.o" ]; then
    pass "-M DIR recurses"
else
    fail "-M DIR recurses" "$(cat "$WORK/t6.log")"
fi

# --- Test 7: reserved type name produces targeted error ------------------
echo 'func double(int $x) int { return $x; }' > "$WORK/bad.strada"
err="$("$STRADAC" "$WORK/bad.strada" "$WORK/bad.c" 2>&1 || true)"
if echo "$err" | grep -q "reserved type name"; then
    pass "reserved-type identifier produces targeted error"
else
    fail "reserved-type identifier produces targeted error" "got: $err"
fi

# --- Test 8: -M auto-deduces lib root from package + filename ------------
# A sub-module compiled via `strada -M` whose own `use` statements need to
# resolve to sibling .strada files in a nested namespace. Without auto-
# deduction, `use Other::Mod;` from `Test/Chain.strada` can't find
# Other/Mod.strada (lib_paths is empty by default), so stradac emits the
# qualified call site with no extern decl. C falls back to implicit
# `int Other_Mod_sum_array()` -- ABI mismatch with the real signature
# (StradaValue *func(StradaValue *)) -- and the call segfaults at runtime
# whenever the function returns or accepts a non-int value.
#
# The fix: in -M mode, compile() reads the source's `package X::Y;`
# directive and the input file path, strips the package's relative
# subpath from the file path, and adds the resulting lib root to
# lib_paths automatically. Then `use Other::Mod;` resolves and the
# extern is emitted with the right signature.
mkdir -p "$WORK/xmod/Other" "$WORK/xmod/Test"
cat > "$WORK/xmod/Other/Source.strada" <<'EOF'
package Other::Source;
func make_array(int $n) array {
    my array @a = ();
    my int $i = 0;
    while ($i < $n) { push(@a, $i * 10); $i = $i + 1; }
    return @a;
}
EOF
cat > "$WORK/xmod/Other/Mod.strada" <<'EOF'
package Other::Mod;
func sum_array(array @arr) int {
    my int $sum = 0;
    my int $i = 0;
    while ($i < scalar(@arr)) { $sum = $sum + $arr[$i]; $i = $i + 1; }
    return $sum;
}
EOF
cat > "$WORK/xmod/Test/Chain.strada" <<'EOF'
package Test::Chain;
use Other::Source;
use Other::Mod;
# Chained cross-module call: receive an array from one module, pass it to
# another. Triggers the StradaValue *-vs-int implicit-decl ABI mismatch
# if the auto-deduce is broken.
func test_chain(int $n) int {
    my array @items = Other::Source::make_array($n);
    return Other::Mod::sum_array(@items);
}
EOF
cat > "$WORK/xmod/main.strada" <<'EOF'
use lib ".";
use Test::Chain;
func main() int {
    my int $r = Test::Chain::test_chain(4);
    if ($r != 60) { say("got " . $r); return 1; }
    say("OK");
    return 0;
}
EOF
cd "$WORK/xmod"
# Build each .o via -M, then link main without explicit -L.
"$STRADA" -M Other/Source.strada >"$WORK/t8a.log" 2>&1
"$STRADA" -M Other/Mod.strada    >"$WORK/t8b.log" 2>&1
"$STRADA" -M Test/Chain.strada   >"$WORK/t8c.log" 2>&1
# main has `use lib ".";` so it can auto-detect Test/Chain.o; Chain.o
# in turn needs Other/Source.o and Other/Mod.o resolved at link, which
# main2's own use chain pulls in only if `use lib` reaches them. To keep
# the test focused on the auto-deduce fix (not on transitive .o linking)
# we pass the helper .o's explicitly.
if "$STRADA" -o app main.strada Other/Source.o Other/Mod.o >"$WORK/t8d.log" 2>&1 && [ -x app ]; then
    out="$(./app)"
    if [ "$out" = "OK" ]; then
        pass "-M auto-deduces lib root for cross-module use"
    else
        fail "-M auto-deduces lib root for cross-module use" "output was: $out (expected OK from 0+10+20+30=60)"
    fi
else
    fail "-M auto-deduces lib root for cross-module use" "build failed: $(cat "$WORK/t8d.log")"
fi
cd "$WORK/proj"

# --- Test 9: -M writes a metadata sidecar; probe is the fallback ---------
rm -f Util.o Util.o.smeta Big.o Big.o.smeta app9
"$STRADA" -M Util.strada >"$WORK/t9a.log" 2>&1
if [ -s Util.o.smeta ] && grep -q '^func:Util_doubled:' Util.o.smeta; then
    pass "-M writes metadata sidecar (.smeta)"
else
    fail "-M writes metadata sidecar (.smeta)" "missing or wrong $PWD/Util.o.smeta"
fi
cat > app9.strada <<'EOF'
use lib ".";
use Util;
func main() int {
    if (Util::doubled(4) != 8) { return 1; }
    say("OK");
    return 0;
}
EOF
if "$STRADA" -o app9 app9.strada >"$WORK/t9b.log" 2>&1 && [ "$(./app9)" = "OK" ]; then
    pass "use with .smeta sidecar compiles and runs"
else
    fail "use with .smeta sidecar compiles and runs" "$(cat "$WORK/t9b.log")"
fi
rm -f Util.o.smeta app9
if "$STRADA" -o app9 app9.strada >"$WORK/t9c.log" 2>&1 && [ "$(./app9)" = "OK" ]; then
    pass "missing sidecar falls back to metadata probe"
else
    fail "missing sidecar falls back to metadata probe" "$(cat "$WORK/t9c.log")"
fi

# --- Test 10: transitive use: deps from artifact metadata ----------------
# main uses ONLY Big; Big's artifact metadata records `use:Util`, so Util
# must be resolved (and its artifact linked) without main ever naming it.
# Before use:-metadata this failed at link with undefined Util_* symbols.
rm -f Util.o Util.o.smeta Big.o Big.o.smeta app10
"$STRADA" -M Util.strada >"$WORK/t10a.log" 2>&1
"$STRADA" -M Big.strada  >"$WORK/t10b.log" 2>&1
if ! grep -q '^use:Util$' Big.o.smeta 2>/dev/null; then
    fail "artifact metadata records use: deps" "no use:Util line in Big.o.smeta"
else
    pass "artifact metadata records use: deps"
fi
cat > app10.strada <<'EOF'
use lib ".";
use Big;
func main() int {
    if (Big::compute(5) != 25) { return 1; }
    say("OK");
    return 0;
}
EOF
if "$STRADA" -o app10 app10.strada >"$WORK/t10c.log" 2>&1 && [ "$(./app10)" = "OK" ]; then
    pass "transitive artifact dep resolved via use: metadata"
else
    fail "transitive artifact dep resolved via use: metadata" "$(cat "$WORK/t10c.log")"
fi

# --- Test 11: module cache directory (no sibling artifacts) --------------
# Artifacts live ONLY under STRADA_MODULE_CACHE_DIR, mirrored by absolute
# source path -- the layout used for read-only lib dirs.
rm -f Util.o Util.o.smeta Big.o Big.o.smeta app11
MC_DIR="$WORK/mcache11"
rm -rf "$MC_DIR"
cache_util="$MC_DIR$(realpath Util.strada).o"
cache_big="$MC_DIR$(realpath Big.strada).o"
mkdir -p "$(dirname "$cache_util")"
"$STRADA" -M --object -o "$cache_util" Util.strada >"$WORK/t11a.log" 2>&1
"$STRADA" -M --object -o "$cache_big"  Big.strada  >"$WORK/t11b.log" 2>&1
v_out="$(STRADA_MODULE_CACHE_DIR="$MC_DIR" "$STRADA" -v -o app11 app10.strada 2>&1)"
if [ -x app11 ] && [ "$(./app11)" = "OK" ] && echo "$v_out" | grep -q "import_object files:.*$MC_DIR"; then
    pass "module cache directory resolves artifacts (no siblings)"
else
    fail "module cache directory resolves artifacts (no siblings)" "$v_out"
fi

# --- Test 12: --module-cache cold warm-up + warm reuse -------------------
rm -f Util.o Util.o.smeta Big.o Big.o.smeta app12
MC2_DIR="$WORK/mcache12"
rm -rf "$MC2_DIR"
cold_out="$(STRADA_MODULE_CACHE_DIR="$MC2_DIR" "$STRADA" --module-cache -o app12 app10.strada 2>&1)"
if [ -x app12 ] && [ "$(./app12)" = "OK" ] && echo "$cold_out" | grep -q "module cache: warmed"; then
    pass "--module-cache cold build warms the cache"
else
    fail "--module-cache cold build warms the cache" "$cold_out"
fi
rm -f app12
warm_out="$(STRADA_MODULE_CACHE_DIR="$MC2_DIR" "$STRADA" --module-cache -v -o app12 app10.strada 2>&1)"
if [ -x app12 ] && [ "$(./app12)" = "OK" ] \
    && echo "$warm_out" | grep -q "import_object files:.*$MC2_DIR" \
    && ! echo "$warm_out" | grep -q "module cache: warmed"; then
    pass "--module-cache warm build reuses cached artifacts"
else
    fail "--module-cache warm build reuses cached artifacts" "$warm_out"
fi

# --- Test 13: module cache is keyed by -D defines -------------------------
# A module whose __C__ code depends on a define must produce DIFFERENT
# cached artifacts per define set (regression: a DBI.o cached without
# -DHAVE_MYSQL poisoned MySQL builds with "unsupported driver").
mkdir -p "$WORK/defproj"
cat > "$WORK/defproj/Feat.strada" <<'EOF'
package Feat;
func enabled() int {
    my int $r = 0;
    __C__ {
#ifdef HAVE_FEAT
        strada_decref(r);
        r = strada_new_int(1);
#endif
    }
    return $r;
}
EOF
cat > "$WORK/defproj/dapp.strada" <<'EOF'
use lib ".";
use Feat;
func main() int {
    say("feat=" . Feat::enabled());
    return 0;
}
EOF
cd "$WORK/defproj"
MC3_DIR="$WORK/mcache13"
rm -rf "$MC3_DIR"
STRADA_MODULE_CACHE_DIR="$MC3_DIR" "$STRADA" --module-cache -o dapp_off dapp.strada >"$WORK/t13a.log" 2>&1
STRADA_MODULE_CACHE_DIR="$MC3_DIR" "$STRADA" --module-cache -D HAVE_FEAT -o dapp_on dapp.strada >"$WORK/t13b.log" 2>&1
# Re-run both warm to ensure cached artifacts are used and still correct.
STRADA_MODULE_CACHE_DIR="$MC3_DIR" "$STRADA" --module-cache -o dapp_off2 dapp.strada >"$WORK/t13c.log" 2>&1
STRADA_MODULE_CACHE_DIR="$MC3_DIR" "$STRADA" --module-cache -D HAVE_FEAT -o dapp_on2 dapp.strada >"$WORK/t13d.log" 2>&1
out_off="$(./dapp_off)"
out_on="$(./dapp_on)"
out_off2="$(./dapp_off2)"
out_on2="$(./dapp_on2)"
if [ "$out_off" = "feat=0" ] && [ "$out_on" = "feat=1" ] \
    && [ "$out_off2" = "feat=0" ] && [ "$out_on2" = "feat=1" ]; then
    pass "module cache keyed by -D defines"
else
    fail "module cache keyed by -D defines" "off=$out_off on=$out_on off2=$out_off2 on2=$out_on2"
fi
cd "$WORK/proj"

# --- Summary --------------------------------------------------------------
echo
echo "===== $((PASS + FAIL)) tests, $PASS passed, $FAIL failed ====="
[ "$FAIL" -eq 0 ]
