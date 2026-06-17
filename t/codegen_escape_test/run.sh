#!/bin/bash
#
# Security regression: the codegen must route EVERY source-derived string into
# generated C through the gen_str_literal_c escaper, never raw-concatenate it
# between C double-quotes. Three sites previously did the raw thing, so a `"`
# (or `\`/newline) in the value closed the C string literal and injected live
# C — arbitrary code execution at compile/build time when compiling untrusted
# source or a file with an adversarial name (CWE-94):
#   1. tie() class-name string literal      (CodeGenBuiltins.strada)
#   2. __FILE__                              (CodeGenExpr.strada)
#   3. #line directive filename, under -g    (CodeGen.strada)
#
# Plus the build-driver shell-injection guard (CWE-78): a poisoned CC env var
# must never reach the /bin/sh -c link/version probes.
#
# Each case uses a payload value containing `");PAYLOAD` so that an UNescaped
# emission yields the bare C token sequence `");PAYLOAD` while a correctly
# escaped one yields `\");PAYLOAD` inside a string literal. We assert the
# escaped form is present and the breakout form is absent, and that the
# generated C still compiles.
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADAC="$REPO_DIR/stradac"
STRADA="$REPO_DIR/strada"

if [ ! -x "$STRADAC" ] || [ ! -x "$STRADA" ]; then
    echo "Build strada first (run 'make' in $REPO_DIR)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# A breakout payload: as a C string literal value it is  A");PAYLOAD;f("B
PAYLOAD_BREAKOUT='A");CODEGEN_INJECT_PAYLOAD;f("B'   # appears iff UNescaped
PAYLOAD_ESCAPED='A\");CODEGEN_INJECT_PAYLOAD'        # appears iff escaped

# ----------------------------------------------------------------------------
# 1. tie() class-name literal
# ----------------------------------------------------------------------------
cat > "$WORK/tie.strada" <<'EOF'
package main;
func main() int {
    my hash %h = ();
    tie(%h, "A\");CODEGEN_INJECT_PAYLOAD;f(\"B", 0);
    return 0;
}
EOF
"$STRADAC" "$WORK/tie.strada" "$WORK/tie.c" >/dev/null 2>&1 || fail "tie: stradac failed"
grep -F -q "$PAYLOAD_BREAKOUT" "$WORK/tie.c" && fail "tie: classname injected UNescaped into C (breakout)"
grep -F -q "$PAYLOAD_ESCAPED"  "$WORK/tie.c" || fail "tie: escaped classname not found (test stale?)"
gcc -fsyntax-only -I"$REPO_DIR/runtime" "$WORK/tie.c" 2>/dev/null || fail "tie: generated C does not compile"
echo "ok: tie() class-name escaped"

# ----------------------------------------------------------------------------
# 2. __FILE__ with a payload in the SOURCE FILENAME
# ----------------------------------------------------------------------------
FDIR="$WORK/fd"; mkdir -p "$FDIR"
FNAME="$FDIR/A\");CODEGEN_INJECT_PAYLOAD;f(\"B.strada"   # literal " in the name
cat > "$FNAME" <<'EOF'
package main;
func main() int { my str $f = __FILE__; return 0; }
EOF
"$STRADAC" "$FNAME" "$WORK/file.c" >/dev/null 2>&1 || fail "__FILE__: stradac failed"
grep -F -q "$PAYLOAD_BREAKOUT" "$WORK/file.c" && fail "__FILE__: filename injected UNescaped into C (breakout)"
grep -F -q "$PAYLOAD_ESCAPED"  "$WORK/file.c" || fail "__FILE__: escaped filename not found (test stale?)"
gcc -fsyntax-only -I"$REPO_DIR/runtime" "$WORK/file.c" 2>/dev/null || fail "__FILE__: generated C does not compile"
echo "ok: __FILE__ filename escaped"

# ----------------------------------------------------------------------------
# 3. #line directive filename under -g (no __FILE__ in the program — isolates
#    the per-statement #line path)
# ----------------------------------------------------------------------------
cat > "$FNAME" <<'EOF'
package main;
func main() int { my int $x = 1; $x = $x + 1; return 0; }
EOF
"$STRADAC" -g "$FNAME" "$WORK/line.c" >/dev/null 2>&1 || fail "#line: stradac -g failed"
grep -q "^#line" "$WORK/line.c" || fail "#line: no #line directives emitted under -g (test stale?)"
grep -F -q "$PAYLOAD_BREAKOUT" "$WORK/line.c" && fail "#line: filename injected UNescaped into C (breakout)"
grep -F -q "$PAYLOAD_ESCAPED"  "$WORK/line.c" || fail "#line: escaped filename not found (test stale?)"
gcc -fsyntax-only -I"$REPO_DIR/runtime" "$WORK/line.c" 2>/dev/null || fail "#line: generated C does not compile"
echo "ok: #line filename escaped"

# ----------------------------------------------------------------------------
# 4. Build driver: poisoned CC must be rejected, injected command must NOT run
# ----------------------------------------------------------------------------
cat > "$WORK/hello.strada" <<'EOF'
package main;
func main() int { say("hello"); return 0; }
EOF
PWNED="$WORK/PWNED"
for poison in 'cc; touch '"$PWNED" 'cc $(touch '"$PWNED"')' 'cc `touch '"$PWNED"'`'; do
    rm -f "$PWNED"
    CC="$poison" "$STRADA" "$WORK/hello.strada" -o "$WORK/hello_poison" >/dev/null 2>&1
    [ -e "$PWNED" ] && fail "driver: CC injection executed ($poison)"
done
echo "ok: driver rejects poisoned CC (no command executed)"

# Sanity: a normal build still works.
"$STRADA" "$WORK/hello.strada" -o "$WORK/hello_ok" >/dev/null 2>&1 || fail "driver: normal build failed"
[ "$("$WORK/hello_ok")" = "hello" ] || fail "driver: normal build produced wrong output"
echo "ok: normal build unaffected"

echo "PASS: codegen-escape + driver CC-injection regression"
exit 0
