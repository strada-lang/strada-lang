#!/bin/bash
# Memory leak tests for the Strada interpreter
# Runs test programs under valgrind and checks for leaks
# Usage: ./run_leak_tests.sh [-v] [-j N] [pattern]

INTERP="${INTERP:-$(dirname "$0")/../../strada-interp}"
INTERP_FLAGS="${INTERP_FLAGS:-}"
TEST_DIR="$(dirname "$0")"
PASS=0
FAIL=0
SKIP=0
VERBOSE=0
JOBS="${STRADA_LEAK_JOBS:-$(nproc 2>/dev/null || echo 4)}"
PATTERN=""

while [ $# -gt 0 ]; do
    case "$1" in
        -v) VERBOSE=1; shift ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
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

TMP_LOG_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_LOG_DIR"' EXIT

SELECTED=()
for test_file in "$TEST_DIR"/leak_*.strada; do
    [ -f "$test_file" ] || continue
    test_name=$(basename "$test_file" .strada)

    if [ -n "$PATTERN" ] && ! echo "$test_name" | grep -q "$PATTERN"; then
        continue
    fi
    SELECTED+=("$test_file")
done

run_interp_leak_one() {
    local test_file="$1"
    local test_name
    test_name=$(basename "$test_file" .strada)
    local out_file="$TMP_LOG_DIR/${test_name}.msg"
    local status_file="$TMP_LOG_DIR/${test_name}.rc"

    local local_output
    local_output=$(valgrind --leak-check=full --errors-for-leak-kinds=definite \
        --error-exitcode=42 \
        $INTERP $INTERP_FLAGS "$test_file" 2>&1)
    local exit_code=$?

    local definitely_lost
    definitely_lost=$(echo "$local_output" | grep "definitely lost:" | sed -E 's/.*definitely lost: ([0-9,]+) bytes.*/\1/' | tr -d ',')

    LEAK_THRESHOLD="${LEAK_THRESHOLD:-0}"

    if [ "$exit_code" -eq 42 ]; then
        if [ -n "$definitely_lost" ] && [ "$definitely_lost" -le "$LEAK_THRESHOLD" ]; then
            echo -e "  ${GREEN}PASS${NC}: $test_name (${definitely_lost} bytes - within threshold)" > "$out_file"
            echo 0 > "$status_file"
        else
            {
                echo -e "  ${RED}LEAK${NC}: $test_name (${definitely_lost:-?} bytes definitely lost)"
                if [ $VERBOSE -eq 1 ]; then
                    echo "$local_output" | grep -A5 "LEAK SUMMARY" | sed 's/^/    /'
                fi
            } > "$out_file"
            echo 1 > "$status_file"
        fi
    elif [ "$exit_code" -ne 0 ]; then
        echo -e "  ${RED}FAIL${NC}: $test_name (exit code $exit_code)" > "$out_file"
        echo 1 > "$status_file"
    else
        echo -e "  ${GREEN}PASS${NC}: $test_name" > "$out_file"
        echo 0 > "$status_file"
    fi
}

PIDS=()
TEST_NAMES=()
for test_file in "${SELECTED[@]}"; do
    test_name=$(basename "$test_file" .strada)
    if [ "${JOBS:-1}" -le 1 ]; then
        run_interp_leak_one "$test_file"
        cat "$TMP_LOG_DIR/${test_name}.msg"
        if [ "$(cat "$TMP_LOG_DIR/${test_name}.rc")" -eq 0 ]; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi
    else
        run_interp_leak_one "$test_file" &
        PIDS+=("$!")
        TEST_NAMES+=("$test_name")
        if [ "${#PIDS[@]}" -ge "$JOBS" ]; then
            wait "${PIDS[0]}"
            n0="${TEST_NAMES[0]}"
            cat "$TMP_LOG_DIR/${n0}.msg"
            if [ "$(cat "$TMP_LOG_DIR/${n0}.rc")" -eq 0 ]; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi
            PIDS=("${PIDS[@]:1}")
            TEST_NAMES=("${TEST_NAMES[@]:1}")
        fi
    fi
done

for idx in "${!PIDS[@]}"; do
    wait "${PIDS[$idx]}"
    n="${TEST_NAMES[$idx]}"
    cat "$TMP_LOG_DIR/${n}.msg"
    if [ "$(cat "$TMP_LOG_DIR/${n}.rc")" -eq 0 ]; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi
done

echo ""
echo "=== Leak Results: ${PASS} clean, ${FAIL} leaking, ${SKIP} errors ==="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
