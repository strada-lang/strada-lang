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

# --- Test 9: -M embeds export metadata in the .strada_meta section -------
# The .o is self-describing: its interface lives in a .strada_meta section
# (read in-process by the consumer). No separate .smeta sidecar is written.
rm -f Util.o Big.o app9
"$STRADA" -M Util.strada >"$WORK/t9a.log" 2>&1
if [ ! -f Util.o.smeta ] \
   && objcopy -O binary --only-section=.strada_meta Util.o /dev/stdout 2>/dev/null | grep -q '^func:Util_doubled:'; then
    pass "-M embeds export metadata in the .strada_meta section (no sidecar)"
else
    fail "-M embeds export metadata in the .strada_meta section (no sidecar)" "no func:Util_doubled in Util.o .strada_meta (or a .smeta was written)"
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
    pass "use reads the interface from the .strada_meta section"
else
    fail "use reads the interface from the .strada_meta section" "$(cat "$WORK/t9b.log")"
fi

# --- Test 10: transitive use: deps from artifact metadata ----------------
# main uses ONLY Big; Big's artifact metadata records `use:Util`, so Util
# must be resolved (and its artifact linked) without main ever naming it.
# Before use:-metadata this failed at link with undefined Util_* symbols.
rm -f Util.o Big.o app10
"$STRADA" -M Util.strada >"$WORK/t10a.log" 2>&1
"$STRADA" -M Big.strada  >"$WORK/t10b.log" 2>&1
if ! objcopy -O binary --only-section=.strada_meta Big.o /dev/stdout 2>/dev/null | grep -q '^use:Util$'; then
    fail "artifact metadata records use: deps" "no use:Util line in Big.o .strada_meta"
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

# --- 17. import_object .o consumed via its .strada_meta section ----------
# A .o referenced by import_object (in a simulated read-only lib tree) is
# self-describing: the compiler reads its .strada_meta section in-process,
# with no .smeta sidecar and no compile-and-run probe. (The probe + its
# result-cache remain only as a fallback for non-ELF artifacts, which can't
# easily be produced on Linux to exercise here.)
mkdir -p "$WORK/probeproj/lib"
cd "$WORK/probeproj"
cat > lib/PCMod.strada <<'EOF'
package PCMod;
func quad(int $x) int { return $x * 4; }
EOF
cat > pchost.strada <<'EOF'
use lib "lib";
import_object "PCMod.o";
func main() int { say("q=" . PCMod::quad(5)); return 0; }
EOF
"$STRADA" -M --object -o lib/PCMod.o lib/PCMod.strada >"$WORK/t17a.log" 2>&1
"$STRADA" pchost.strada -o pchost lib/PCMod.o >"$WORK/t17b.log" 2>&1
out17="$(./pchost)"
"$STRADA" pchost.strada -o pchost2 lib/PCMod.o >"$WORK/t17c.log" 2>&1
out17b="$(./pchost2)"
if [ "$out17" = "q=20" ] && [ "$out17b" = "q=20" ] && [ ! -f lib/PCMod.o.smeta ]; then
    pass "import_object .o consumed via .strada_meta section (no sidecar/probe)"
else
    fail "import_object .o consumed via .strada_meta section (no sidecar/probe)" "out1=$out17 out2=$out17b"
fi

# --- 18. shebanged module survives -D (stradapp passthrough) + warming ----
# #!/usr/bin/env strada on line 1 is valid Strada; stradapp used to die
# on it as an unknown directive, which silently broke module-cache
# warming for any shebanged helper module in a -D build.
mkdir -p "$WORK/sheproj/lib"
cd "$WORK/sheproj"
printf '#!/usr/bin/env strada\npackage SheMod;\nfunc five() int { return 5; }\n' > lib/SheMod.strada
cat > shehost.strada <<'EOF'
use lib "lib";
use SheMod;
func main() int { say("f=" . SheMod::five()); return 0; }
EOF
SHE_DIR="$WORK/shecache"
rm -rf "$SHE_DIR"
STRADA_MODULE_CACHE_DIR="$SHE_DIR" "$STRADA" --module-cache -D SOME_FLAG shehost.strada -o shehost >"$WORK/t18a.log" 2>&1
out18="$(./shehost 2>/dev/null)"
# The warmer must have produced an artifact for the shebanged module.
she_warmed="$(find "$SHE_DIR" -name 'SheMod.strada.o' 2>/dev/null | wc -l)"
if [ "$out18" = "f=5" ] && [ "$she_warmed" -ge 1 ]; then
    pass "shebang module: stradapp passthrough + cache warming"
else
    fail "shebang module: stradapp passthrough + cache warming" "out=$out18 warmed=$she_warmed (see t18a.log)"
fi
cd "$WORK/proj"

# --- Test 19: -M auto-extern for an undeclared cross-module call ----------
# Regression: a module compiled with -M that calls a function it neither
# defines nor `use`s (resolved only at the final link) must emit
# `extern StradaValue* name();`. Without it, C defaults the call to `int
# name()` and TRUNCATES the returned 64-bit StradaValue* pointer to 32 bits.
# A heap return (a string) then becomes a garbage pointer -> segfault. (An
# int return like 25 would survive truncation as a tagged int, which is why
# the earlier end-to-end test couldn't catch this.) Relay deliberately omits
# `use StrLib`, so StrLib::banner is unknown to Relay's translation unit.
mkdir -p "$WORK/ax"
cat > "$WORK/ax/StrLib.strada" <<'EOF'
package StrLib;
func banner(int $n) {                 # no return type -> heap string
    my str $s = "[";
    my int $i = 0;
    while ($i < $n) { $s = $s . "="; $i = $i + 1; }
    return $s . "]";
}
EOF
cat > "$WORK/ax/Relay.strada" <<'EOF'
package Relay;                        # NOTE: no `use StrLib;`
func get_banner() { return StrLib::banner(20); }
EOF
cat > "$WORK/ax/app.strada" <<'EOF'
use lib ".";
use Relay;
use StrLib;
func main() int {
    my str $b = Relay::get_banner();
    if (length($b) == 22) { say("OK"); return 0; }
    say("CORRUPT len=" . length($b)); return 1;
}
EOF
cd "$WORK/ax"
ax_ok=0
out19=""; rc19=""
if "$STRADA" -M StrLib.strada >"$WORK/t19a.log" 2>&1 \
   && "$STRADA" -M Relay.strada >"$WORK/t19b.log" 2>&1 \
   && "$STRADA" -o app app.strada >"$WORK/t19c.log" 2>&1 && [ -x app ]; then
    out19="$(./app 2>/dev/null)"; rc19=$?
    if [ "$out19" = "OK" ] && [ "$rc19" -eq 0 ]; then ax_ok=1; fi
fi
if [ "$ax_ok" -eq 1 ]; then
    pass "-M emits extern for undeclared cross-module call (no pointer truncation)"
else
    fail "-M emits extern for undeclared cross-module call (no pointer truncation)" \
         "app output='${out19}' rc='${rc19}' (segfault/CORRUPT => returned StradaValue* truncated to int)"
fi
cd "$WORK/proj"

# --- Test 20: passing a Strada-module .so on the command line -------------
# A .so can be supplied as a positional arg or via --import-lib; both are
# treated as an implicit `import_lib` (dlopen'd at runtime), so the app can
# call into it WITHOUT a source-level `import_lib` directive. (.o/.a are
# linked statically; a .so is loaded at runtime — see do_import_lib_at.)
mkdir -p "$WORK/so"
cat > "$WORK/so/MyLib.strada" <<'EOF'
package MyLib;
func greet(str $name) { return "Hello, " . $name . "!"; }
func add(int $a, int $b) int { return $a + $b; }
EOF
cat > "$WORK/so/app.strada" <<'EOF'
func main() int {              # NOTE: no `import_lib` directive
    say(MyLib::greet("world"));
    say("add=" . MyLib::add(40, 2));
    return 0;
}
EOF
cd "$WORK/so"
so_ok=0
out20a=""; out20b=""
if "$STRADA" --shared MyLib.strada -o MyLib.so >"$WORK/t20build.log" 2>&1 && [ -f MyLib.so ]; then
    # form 1: positional .so
    if "$STRADA" -o app1 app.strada "$WORK/so/MyLib.so" >"$WORK/t20a.log" 2>&1 && [ -x app1 ]; then
        out20a="$(./app1 2>/dev/null)"
    fi
    # form 2: --import-lib flag
    if "$STRADA" --import-lib "$WORK/so/MyLib.so" -o app2 app.strada >"$WORK/t20b.log" 2>&1 && [ -x app2 ]; then
        out20b="$(./app2 2>/dev/null)"
    fi
fi
expected=$'Hello, world!\nadd=42'
if [ "$out20a" = "$expected" ] && [ "$out20b" = "$expected" ]; then
    so_ok=1
fi
if [ "$so_ok" -eq 1 ]; then
    pass "CLI .so (positional + --import-lib) imported as runtime import_lib"
else
    fail "CLI .so (positional + --import-lib) imported as runtime import_lib" \
         "positional='${out20a}' --import-lib='${out20b}' (want 'Hello, world!' / 'add=42')"
fi
cd "$WORK/proj"

# --- Test 21: in-process .strada_meta read across a heap-returning call ---
# End-to-end exercise of the in-process section reader: a consumer resolves
# a .o's interface purely from its .strada_meta section (no sidecar, no
# probe) and correctly calls a function returning a heap string (a pointer
# the metadata path must convey accurately). Util::banner has no declared
# return type, so it returns a heap string across the artifact boundary.
mkdir -p "$WORK/sec"
cat > "$WORK/sec/Util.strada" <<'EOF'
package Util;
func banner(int $n) { my str $s = "["; my int $i = 0; while ($i < $n) { $s = $s . "="; $i = $i + 1; } return $s . "]"; }
EOF
cat > "$WORK/sec/app.strada" <<'EOF'
use lib ".";
use Util;
func main() int {
    my str $b = Util::banner(20);
    if (length($b) == 22) { say("OK"); return 0; }
    say("BAD len=" . length($b)); return 1;
}
EOF
cd "$WORK/sec"
sec_ok=0; sec_out=""
if "$STRADA" -M Util.strada >"$WORK/t21a.log" 2>&1 && [ ! -f Util.o.smeta ] \
   && "$STRADA" -o app app.strada >"$WORK/t21b.log" 2>&1 && [ -x app ]; then
    sec_out="$(./app 2>/dev/null)"
    [ "$sec_out" = "OK" ] && sec_ok=1
fi
if [ "$sec_ok" -eq 1 ]; then
    pass "in-process .strada_meta read resolves a heap-returning cross-artifact call"
else
    fail "in-process .strada_meta read resolves a heap-returning cross-artifact call" "out='${sec_out}'"
fi
cd "$WORK/proj"

# --- Summary --------------------------------------------------------------
echo
echo "===== $((PASS + FAIL)) tests, $PASS passed, $FAIL failed ====="
[ "$FAIL" -eq 0 ]
