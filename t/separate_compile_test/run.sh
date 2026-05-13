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

# --- Summary --------------------------------------------------------------
echo
echo "===== $((PASS + FAIL)) tests, $PASS passed, $FAIL failed ====="
[ "$FAIL" -eq 0 ]
