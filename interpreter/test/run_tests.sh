#!/bin/bash
# Test runner for the Strada interpreter
# Usage: ./run_tests.sh [-v] [pattern]
#   -v        Verbose output
#   pattern   Only run tests matching pattern

INTERP="$(dirname "$0")/../strada-interp"
TEST_DIR="$(dirname "$0")"
PASS=0
FAIL=0
SKIP=0
VERBOSE=0
PATTERN=""

# Parse args
while [ $# -gt 0 ]; do
    case "$1" in
        -v) VERBOSE=1; shift ;;
        *)  PATTERN="$1"; shift ;;
    esac
done

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

# Run a test that checks output
# test_output <name> <file> <expected_output>
test_output() {
    local name="$1"
    local file="$2"
    local expected="$3"

    if [ -n "$PATTERN" ] && ! echo "$name" | grep -q "$PATTERN"; then
        return
    fi

    local actual
    actual=$($INTERP "$file" 2>&1)
    local exit_code=$?

    if [ "$actual" = "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC}: $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $name"
        if [ $VERBOSE -eq 1 ]; then
            echo "    expected: $(echo "$expected" | head -3)"
            echo "    actual:   $(echo "$actual" | head -3)"
        fi
        FAIL=$((FAIL + 1))
    fi
}

# Run a test that checks output contains a string
# test_output_contains <name> <file> <expected_substring>
test_output_contains() {
    local name="$1"
    local file="$2"
    local expected="$3"

    if [ -n "$PATTERN" ] && ! echo "$name" | grep -q "$PATTERN"; then
        return
    fi

    local actual
    actual=$($INTERP "$file" 2>&1)

    if echo "$actual" | grep -qF "$expected"; then
        echo -e "  ${GREEN}PASS${NC}: $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $name"
        if [ $VERBOSE -eq 1 ]; then
            echo "    expected to contain: $expected"
            echo "    actual: $(echo "$actual" | head -5)"
        fi
        FAIL=$((FAIL + 1))
    fi
}

# Run a test that checks exit code
# test_exit_code <name> <file> <expected_code>
test_exit_code() {
    local name="$1"
    local file="$2"
    local expected="$3"

    if [ -n "$PATTERN" ] && ! echo "$name" | grep -q "$PATTERN"; then
        return
    fi

    $INTERP "$file" > /dev/null 2>&1
    local actual=$?

    if [ "$actual" -eq "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC}: $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $name"
        if [ $VERBOSE -eq 1 ]; then
            echo "    expected exit code: $expected, got: $actual"
        fi
        FAIL=$((FAIL + 1))
    fi
}

# Run a test that should compile and run without error
# test_run <name> <file>
test_run() {
    local name="$1"
    local file="$2"

    if [ -n "$PATTERN" ] && ! echo "$name" | grep -q "$PATTERN"; then
        return
    fi

    local actual
    actual=$($INTERP "$file" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo -e "  ${GREEN}PASS${NC}: $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $name (exit code $exit_code)"
        if [ $VERBOSE -eq 1 ]; then
            echo "    output: $(echo "$actual" | head -5)"
        fi
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Strada Interpreter Test Suite ==="
echo ""

# Discover and run all test files
for test_file in "$TEST_DIR"/test_*.strada; do
    [ -f "$test_file" ] || continue
    test_name=$(basename "$test_file" .strada)

    if [ -n "$PATTERN" ] && ! echo "$test_name" | grep -q "$PATTERN"; then
        continue
    fi

    # Check for expected output file
    expected_file="${test_file%.strada}.expected"
    if [ -f "$expected_file" ]; then
        expected=$(cat "$expected_file")
        test_output "$test_name" "$test_file" "$expected"
    else
        # Just test that it runs
        test_run "$test_name" "$test_file"
    fi
done

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
