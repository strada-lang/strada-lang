#!/bin/bash
# Memory leak tests for the Strada interpreter
# Runs test programs under valgrind and checks for leaks
# Usage: ./run_leak_tests.sh [-v] [pattern]

INTERP="${INTERP:-$(dirname "$0")/../../strada-interp}"
INTERP_FLAGS="${INTERP_FLAGS:-}"
TEST_DIR="$(dirname "$0")"
PASS=0
FAIL=0
SKIP=0
VERBOSE=0
PATTERN=""

while [ $# -gt 0 ]; do
    case "$1" in
        -v) VERBOSE=1; shift ;;
        *)  PATTERN="$1"; shift ;;
    esac
done

# Check for valgrind
if ! command -v valgrind &> /dev/null; then
    echo "valgrind not found - skipping leak tests"
    exit 0
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo "=== Strada Interpreter Leak Tests ==="
echo ""

for test_file in "$TEST_DIR"/leak_*.strada; do
    [ -f "$test_file" ] || continue
    test_name=$(basename "$test_file" .strada)

    if [ -n "$PATTERN" ] && ! echo "$test_name" | grep -q "$PATTERN"; then
        continue
    fi

    # Run under valgrind
    local_output=$(valgrind --leak-check=full --errors-for-leak-kinds=definite \
        --error-exitcode=42 \
        $INTERP $INTERP_FLAGS "$test_file" 2>&1)
    exit_code=$?

    # Extract leak summary
    definitely_lost=$(echo "$local_output" | grep "definitely lost:" | sed -E 's/.*definitely lost: ([0-9,]+) bytes.*/\1/' | tr -d ',')

    LEAK_THRESHOLD="${LEAK_THRESHOLD:-0}"

    if [ "$exit_code" -eq 42 ]; then
        if [ -n "$definitely_lost" ] && [ "$definitely_lost" -le "$LEAK_THRESHOLD" ]; then
            echo -e "  ${GREEN}PASS${NC}: $test_name (${definitely_lost} bytes - within threshold)"
            PASS=$((PASS + 1))
        else
            echo -e "  ${RED}LEAK${NC}: $test_name (${definitely_lost:-?} bytes definitely lost)"
            if [ $VERBOSE -eq 1 ]; then
                echo "$local_output" | grep -A5 "LEAK SUMMARY" | sed 's/^/    /'
            fi
            FAIL=$((FAIL + 1))
        fi
    elif [ "$exit_code" -ne 0 ]; then
        echo -e "  ${RED}FAIL${NC}: $test_name (exit code $exit_code)"
        FAIL=$((FAIL + 1))
    else
        echo -e "  ${GREEN}PASS${NC}: $test_name"
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "=== Leak Results: ${PASS} clean, ${FAIL} leaking, ${SKIP} errors ==="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
