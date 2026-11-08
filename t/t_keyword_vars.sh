#!/bin/bash
# t_keyword_vars.sh - $until, $unless and $do are ordinary variable names
# (they were "expected a variable name"), and the statements still work.
TOTAL=$((TOTAL + 1))
kv_src="$BUILD_DIR/keyword_vars.strada"
cat > "$kv_src" <<'SRC'
func main() int {
    my int $until = 3;
    my str $unless = "u";
    my scalar $do = [1, 2];
    my int $n = 0;
    until ($n >= $until) { $n = $n + 1; }
    unless ($unless eq "x") { $n = $n + 10; }
    do { $n = $n + 100; } while ($n < 0);
    say($n . " " . scalar(@{$do}));
    return 0;
}
SRC
kv_out=""
if "$PROJECT_DIR/strada" "$kv_src" -o "$BUILD_DIR/keyword_vars" > "$BUILD_DIR/keyword_vars.log" 2>&1; then kv_out=$("$BUILD_DIR/keyword_vars" 2>&1); fi
if [ "$kv_out" = "113 2" ]; then
    PASSED=$((PASSED + 1)); log_pass "\$until, \$unless and \$do are variable names"
else
    FAILED=$((FAILED + 1)); log_fail "\$until, \$unless and \$do are variable names" "got: $kv_out $(head -c 200 "$BUILD_DIR/keyword_vars.log")"
fi
