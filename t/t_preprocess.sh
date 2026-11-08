#!/bin/bash
# t_preprocess.sh - with -D (the preprocessor runs first), diagnostics still
# name the original file and line: comments, directives and inactive
# #ifdef branches must not shift the line numbers.
TOTAL=$((TOTAL + 1))
pp_src="$BUILD_DIR/pp_lines.strada"
cat > "$pp_src" <<'SRC'
#define FOO 1
# a comment line
#ifdef BAR
func gone() int { return 1; }
#endif
func main() int {
    return nosuch_fn();
}
SRC
pp_out=$("$PROJECT_DIR/strada" -DX "$pp_src" -o "$BUILD_DIR/pp_lines" 2>&1)
if echo "$pp_out" | grep -q "pp_lines.strada:7: error: undefined function 'nosuch_fn'"; then
    PASSED=$((PASSED + 1)); log_pass "Preprocessed diagnostics keep the original file and line"
else
    FAILED=$((FAILED + 1)); log_fail "Preprocessed diagnostics keep the original file and line" "got: $(echo "$pp_out" | head -2)"
fi
