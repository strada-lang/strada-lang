#!/bin/bash
#
# Diagnostics regression: compiler errors must carry a source location.
#
#   1. Lexer errors report file:line (unterminated string), and file:line:col
#      where a column is known (unexpected character).
#   2. Parse errors inside a use'd module report the MODULE's path (the
#      module sub-parser historically had no filename, so these fell back to
#      a bare "line N:" with no hint of which file).
#   3. Semantic errors and warnings report file:line — including for
#      functions inlined from use'd modules, whose AST nodes are stamped
#      with their defining file at merge time.
#
# Each case compiles a fixture expected to fail (or warn) and asserts the
# diagnostic names the right file (and line/column). Exits non-zero on any
# failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"

if [ ! -x "$STRADA" ]; then
    echo "FAIL: strada driver not found at $STRADA"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FAILURES=0

# check <name> <expected-substring> <output>
check() {
    local name="$1" expect="$2" output="$3"
    if printf '%s' "$output" | grep -qF "$expect"; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        echo "  expected substring: $expect"
        echo "  got: $(printf '%s' "$output" | head -3)"
        FAILURES=$((FAILURES + 1))
    fi
}

# --- 1. Lexer: unterminated string in the main file (file:line) ----------
cat > "$WORK/lex_unterm.strada" <<'EOF'
func main() int {
    my str $s = "never closed
    return 0;
}
EOF
out="$("$STRADA" "$WORK/lex_unterm.strada" 2>&1)"
check "lexer unterminated string has file:line:col" \
    "lex_unterm.strada:2:17: Unterminated string" "$out"

# --- 2. Lexer: unexpected character (file:line:col) ----------------------
cat > "$WORK/lex_char.strada" <<'EOF'
func main() int {
    my int $a = 3 ` 4;
    return 0;
}
EOF
out="$("$STRADA" "$WORK/lex_char.strada" 2>&1)"
check "lexer unexpected char has file:line:col" \
    "lex_char.strada:2:19: Unexpected character" "$out"
check "lexer error shows source line" \
    'my int $a = 3 ` 4;' "$out"
if printf '%s\n' "$out" | grep -qE '^ {18}\^$'; then
    echo "PASS: lexer caret aligned under column 19"
else
    echo "FAIL: lexer caret aligned under column 19"
    FAILURES=$((FAILURES + 1))
fi

# --- 3. Lexer error inside a use'd module names the module ---------------
mkdir -p "$WORK/mlib"
cat > "$WORK/mlib/LexMod.strada" <<'EOF'
package LexMod;

func broken() str {
    return "no closing quote
}
EOF
cat > "$WORK/use_lexmod.strada" <<EOF
use lib "$WORK/mlib";
use LexMod;
func main() int { return 0; }
EOF
out="$("$STRADA" "$WORK/use_lexmod.strada" 2>&1)"
check "lexer error in module names module file" \
    "LexMod.strada:4:12: Unterminated string" "$out"

# --- 4. Parse error inside a use'd module names the module ---------------
cat > "$WORK/mlib/ParseMod.strada" <<'EOF'
package ParseMod;

func broken( int {
    return 1;
}
EOF
cat > "$WORK/use_parsemod.strada" <<EOF
use lib "$WORK/mlib";
use ParseMod;
func main() int { return 0; }
EOF
out="$("$STRADA" "$WORK/use_parsemod.strada" 2>&1)"
check "parse error in module names module file:line:col" \
    "ParseMod.strada:3:18:" "$out"
if printf '%s\n' "$out" | grep -qE '^ {17}\^$'; then
    echo "PASS: parser caret aligned under column 18"
else
    echo "FAIL: parser caret aligned under column 18"
    FAILURES=$((FAILURES + 1))
fi

# --- 5. Semantic error in the main file (file:line: error:) --------------
cat > "$WORK/sem_main.strada" <<'EOF'
func main() int {
    my int $y = $oops;
    return 0;
}
EOF
out="$("$STRADA" "$WORK/sem_main.strada" 2>&1)"
check "semantic error has file:line" \
    "sem_main.strada:2: error: undefined variable '\$oops'" "$out"

# --- 6. Semantic error inside a use'd module names the module ------------
cat > "$WORK/mlib/SemMod.strada" <<'EOF'
package SemMod;

func helper() int {
    return $nosuch_var + 1;
}
EOF
cat > "$WORK/use_semmod.strada" <<EOF
use lib "$WORK/mlib";
use SemMod;
func main() int { return 0; }
EOF
out="$("$STRADA" "$WORK/use_semmod.strada" 2>&1)"
check "semantic error in module names module file" \
    "SemMod.strada:4: error: undefined variable" "$out"
check "semantic error shows source line" \
    'return $nosuch_var + 1;' "$out"

# --- 7. Warning (-w) has file:line ----------------------------------------
cat > "$WORK/warn_unused.strada" <<'EOF'
func main() int {
    my int $unused_thing = 5;
    return 0;
}
EOF
out="$("$STRADA" -w "$WORK/warn_unused.strada" 2>&1)"
check "warning has file:line" \
    "warn_unused.strada:2: warning: unused variable" "$out"

# --- 8. Multiple semantic errors reported in one compile -----------------
cat > "$WORK/multi_sem.strada" <<'EOF'
func alpha() int {
    return $missing_one;
}

func beta() int {
    return $missing_two;
}

func main() int {
    return alpha() + beta();
}
EOF
out="$("$STRADA" "$WORK/multi_sem.strada" 2>&1)"
check "multiple semantic errors: first reported" \
    "multi_sem.strada:2: error: undefined variable '\$missing_one'" "$out"
check "multiple semantic errors: second reported" \
    "multi_sem.strada:6: error: undefined variable '\$missing_two'" "$out"
check "multiple semantic errors: summary count" \
    "compilation failed: 2 semantic errors" "$out"

# --- 9. Parse recovery: several broken functions, one compile -------------
cat > "$WORK/multi_parse.strada" <<'EOF'
func broken_one( int {
    return 1;
}

func fine() int {
    return 2;
}

func broken_two() int {
    my int $x = ;
    return $x;
}

func main() int {
    return fine();
}
EOF
out="$("$STRADA" "$WORK/multi_parse.strada" 2>&1)"
check "parse recovery: first error reported" \
    "multi_parse.strada:1:" "$out"
check "parse recovery: second error reported" \
    "multi_parse.strada:10:" "$out"
check "parse recovery: summary count" \
    "compilation failed: 2 parse errors" "$out"

# --- 10. --check: clean file exits 0 and produces no artifacts ------------
cat > "$WORK/check_clean.strada" <<'EOF'
func main() int {
    say("ok");
    return 0;
}
EOF
if "$STRADA" --check "$WORK/check_clean.strada" > "$WORK/check_out.txt" 2>&1; then
    if [ ! -e "$WORK/check_clean" ] && [ ! -e "$WORK/check_clean.c" ]; then
        echo "PASS: --check clean exits 0, no artifacts"
    else
        echo "FAIL: --check clean left artifacts"
        FAILURES=$((FAILURES + 1))
    fi
else
    echo "FAIL: --check clean exited non-zero"
    cat "$WORK/check_out.txt" | head -3
    FAILURES=$((FAILURES + 1))
fi

# --- 11. --check: broken file exits non-zero with located errors ----------
if "$STRADA" --check "$WORK/multi_sem.strada" > "$WORK/check_bad.txt" 2>&1; then
    echo "FAIL: --check on broken file exited 0"
    FAILURES=$((FAILURES + 1))
else
    out="$(cat "$WORK/check_bad.txt")"
    check "--check broken file reports located errors" \
        "multi_sem.strada:2: error:" "$out"
fi

echo ""
if [ $FAILURES -gt 0 ]; then
    echo "diagnostics: $FAILURES case(s) FAILED"
    exit 1
fi
echo "diagnostics: all cases passed"
exit 0
