#!/bin/bash
# t_cblock_lines.sh - newlines inside __C__ blocks (top level and inside a
# function, in code, strings and // comments) are counted once, so later
# diagnostics name the right line (they were counted twice).
TOTAL=$((TOTAL + 1))
cl_src="$BUILD_DIR/cblock_lines.strada"
cat > "$cl_src" <<'SRC'
package P;
__C__ {
#include <stdio.h>
static const char *s = "a\
b";
// a comment
static int f1(void) {
    return 1;
}
}
func g() int {
    my int $r = 0;
    __C__ {
        int x = 1;
        x++;
    }
    return $r;
}
func num(num $v) str { return "x"; }
func main() int { return 0; }
SRC
cl_out=$("$PROJECT_DIR/strada" "$cl_src" -o "$BUILD_DIR/cblock_lines" 2>&1)
if echo "$cl_out" | grep -q "cblock_lines.strada:19:6: 'num' is a reserved type name"; then
    PASSED=$((PASSED + 1)); log_pass "Lines after __C__ blocks are numbered right"
else
    FAILED=$((FAILED + 1)); log_fail "Lines after __C__ blocks are numbered right" "got: $(echo "$cl_out" | head -1)"
fi
