#!/bin/bash
# Quick smoke test for the interpreter

cd "$(dirname "$0")"
INTERP="./strada-interp"
PASS=0
FAIL=0

run_test() {
    local name="$1"
    local input="$2"
    local expected="$3"

    local actual
    actual=$(echo "$input" | $INTERP 2>&1 | grep -v "^Strada Interpreter" | grep -v "^Type expressions" | grep -v "^$" | grep -v "^strada>")

    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    expected: $expected"
        echo "    actual:   $actual"
        FAIL=$((FAIL + 1))
    fi
}

run_file_test() {
    local name="$1"
    local file="$2"
    local expected="$3"

    local actual
    actual=$($INTERP "$file" 2>&1)

    if echo "$actual" | grep -q "$expected"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    expected to contain: $expected"
        echo "    actual: $(echo "$actual" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Strada Interpreter Tests ==="
echo ""

# File execution tests
echo "--- File Execution ---"
run_file_test "example.strada" "../examples/example.strada" "Fibonacci of 10"
run_file_test "control_flow_demo" "../examples/control_flow_demo.strada" "All control flow working"
run_file_test "array_ops" "../examples/array_ops.strada" "Push and Pop"

# Inline program tests
echo ""
echo "--- Inline Programs ---"

cat > /tmp/si_test_arith.strada << 'EOF'
func main() int {
    say(2 + 3);
    say(10 - 4);
    say(3 * 7);
    say(15 / 3);
    say(17 % 5);
    return 0;
}
EOF
run_file_test "arithmetic" "/tmp/si_test_arith.strada" "5"

cat > /tmp/si_test_str.strada << 'EOF'
func main() int {
    my str $s = "Hello" . ", " . "World!";
    say($s);
    say(length($s));
    say(uc($s));
    say(substr($s, 0, 5));
    return 0;
}
EOF
run_file_test "strings" "/tmp/si_test_str.strada" "Hello, World!"

cat > /tmp/si_test_closure.strada << 'EOF'
func make_adder(int $n) scalar {
    return func (int $x) int { return $x + $n; };
}
func main() int {
    my scalar $add5 = make_adder(5);
    say($add5->(10));
    say($add5->(20));
    return 0;
}
EOF
run_file_test "closures" "/tmp/si_test_closure.strada" "15"

cat > /tmp/si_test_trycatch.strada << 'EOF'
func main() int {
    try {
        throw("boom");
    } catch ($e) {
        say("caught: " . $e);
    }
    return 0;
}
EOF
run_file_test "try/catch" "/tmp/si_test_trycatch.strada" "caught: boom"

cat > /tmp/si_test_foreach.strada << 'EOF'
func main() int {
    my array @items = ("a", "b", "c");
    foreach my str $item (@items) {
        print($item . " ");
    }
    say("");
    return 0;
}
EOF
run_file_test "foreach" "/tmp/si_test_foreach.strada" "a b c"

cat > /tmp/si_test_mapgrep.strada << 'EOF'
func main() int {
    my array @nums = (1, 2, 3, 4, 5);
    my scalar $squared = map { $_ * $_; } @nums;
    say(join(" ", $squared));
    my scalar $odds = grep { $_ % 2 != 0; } @nums;
    say(join(" ", $odds));
    return 0;
}
EOF
run_file_test "map/grep" "/tmp/si_test_mapgrep.strada" "1 4 9 16 25"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
