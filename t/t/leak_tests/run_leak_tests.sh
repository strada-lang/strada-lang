#!/bin/bash
# Run all leak tests with valgrind
# Usage: ./run_leak_tests.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STRADA_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$STRADA_DIR"

# Tests that currently pass without memory leaks
TESTS=(
    "test_digest_sha"
    "test_digest_md5"
    "test_crypt"
    "test_random"
    "test_combined"
    "test_strings"
    "test_math"
    "test_oop"
    "test_arrays"
    "test_hashes"
    "test_closures"
    "test_exceptions"
)

# Tests with known leaks (for future work):
# - test_encoding: JSON library has leaks

PASSED=0
FAILED=0

echo "========================================"
echo "Running Leak Tests"
echo "========================================"
echo ""

for test in "${TESTS[@]}"; do
    echo "----------------------------------------"
    echo "Building: $test"
    echo "----------------------------------------"

    # Determine extra linker flags based on test
    EXTRA_FLAGS=""
    case "$test" in
        test_crypt|test_combined)
            EXTRA_FLAGS="-lcrypt"
            ;;
    esac

    if ! ./strada "t/leak_tests/${test}.strada" -o "t/leak_tests/${test}" $EXTRA_FLAGS 2>&1; then
        echo "FAIL: Compilation failed for $test"
        FAILED=$((FAILED + 1))
        continue
    fi

    echo ""
    echo "Running with valgrind..."
    echo ""

    # Run with valgrind and capture output
    VALGRIND_OUT=$(valgrind --leak-check=full --error-exitcode=99 "t/leak_tests/${test}" 2>&1)
    VALGRIND_EXIT=$?

    # Show program output
    echo "$VALGRIND_OUT" | grep -v "^==" || true
    echo ""

    # Check for leaks
    DEFINITELY_LOST=$(echo "$VALGRIND_OUT" | grep "definitely lost:" | awk '{print $4}')
    INDIRECTLY_LOST=$(echo "$VALGRIND_OUT" | grep "indirectly lost:" | awk '{print $4}')

    if [[ "$DEFINITELY_LOST" == "0" ]] && [[ "$INDIRECTLY_LOST" == "0" ]]; then
        echo "LEAK CHECK: PASS (no leaks detected)"
        PASSED=$((PASSED + 1))
    else
        echo "LEAK CHECK: FAIL"
        echo "  Definitely lost: $DEFINITELY_LOST bytes"
        echo "  Indirectly lost: $INDIRECTLY_LOST bytes"
        echo ""
        echo "Valgrind summary:"
        echo "$VALGRIND_OUT" | grep -A5 "LEAK SUMMARY"
        FAILED=$((FAILED + 1))
    fi
    echo ""
done

echo "========================================"
echo "Summary: $PASSED passed, $FAILED failed"
echo "========================================"

# Cleanup binaries
for test in "${TESTS[@]}"; do
    rm -f "t/leak_tests/${test}"
done

if [[ $FAILED -gt 0 ]]; then
    exit 1
fi
exit 0
