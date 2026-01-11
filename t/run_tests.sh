#!/bin/bash
#
# Strada Test Suite Runner
#
# Usage: ./t/run_tests.sh [options] [test_pattern]
#
# Options:
#   -v, --verbose    Show detailed output
#   -k, --keep       Keep generated files
#   -t, --tap        Output in TAP format
#   -c, --compile    Only run compile tests
#   -h, --help       Show this help
#
# Examples:
#   ./t/run_tests.sh                    # Run all tests
#   ./t/run_tests.sh -v test_strings    # Run test_strings with verbose output
#   ./t/run_tests.sh -t                 # TAP format for CI/CD
#

# Note: Don't use set -e, as we need to handle non-zero exit codes in tests

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
STRADAC="$PROJECT_DIR/stradac"
RUNTIME="$PROJECT_DIR/runtime/strada_runtime.o"
RUNTIME_H="$PROJECT_DIR/runtime"
EXAMPLES_DIR="$PROJECT_DIR/examples"
BUILD_DIR="/tmp/strada_tests_$$"

# Source config.sh for optional dependency flags (PCRE2, etc.)
if [ -f "$PROJECT_DIR/config.sh" ]; then
    . "$PROJECT_DIR/config.sh"
fi

# Set EXTRA_LDFLAGS for optional libraries linked into the runtime
EXTRA_LDFLAGS=""
if [ "${STRADA_HAVE_PCRE2:-0}" = "1" ]; then
    EXTRA_LDFLAGS="$EXTRA_LDFLAGS $STRADA_PCRE2_LIBS"
fi

# Memory limit for compiler processes (in KB). Prevents runaway memory from
# killing the system. 2 GB should be more than enough for any test program.
STRADAC_MEM_LIMIT_KB=$((2 * 1024 * 1024))  # 2 GB

# Colors (disabled if not a terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    NC='\033[0m' # No Color
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

# Options
VERBOSE=0
KEEP_FILES=0
TAP_FORMAT=0
COMPILE_ONLY=0
TEST_PATTERN=""

# Statistics
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -k|--keep)
            KEEP_FILES=1
            shift
            ;;
        -t|--tap)
            TAP_FORMAT=1
            shift
            ;;
        -c|--compile)
            COMPILE_ONLY=1
            shift
            ;;
        -h|--help)
            head -20 "$0" | tail -19
            exit 0
            ;;
        *)
            TEST_PATTERN="$1"
            shift
            ;;
    esac
done

# Create build directory
mkdir -p "$BUILD_DIR"

# Cleanup function
cleanup() {
    if [ $KEEP_FILES -eq 0 ]; then
        rm -rf "$BUILD_DIR"
    else
        echo "Test files kept in: $BUILD_DIR"
    fi
}
trap cleanup EXIT

# Logging functions
log_info() {
    if [ $TAP_FORMAT -eq 0 ]; then
        echo -e "${BLUE}[INFO]${NC} $1"
    fi
}

log_pass() {
    if [ $TAP_FORMAT -eq 1 ]; then
        echo "ok $TOTAL - $1"
    else
        echo -e "${GREEN}[PASS]${NC} $1"
    fi
}

log_fail() {
    if [ $TAP_FORMAT -eq 1 ]; then
        echo "not ok $TOTAL - $1"
        if [ -n "$2" ]; then
            echo "  # $2"
        fi
    else
        echo -e "${RED}[FAIL]${NC} $1"
        if [ -n "$2" ]; then
            echo -e "       ${RED}$2${NC}"
        fi
    fi
}

log_skip() {
    if [ $TAP_FORMAT -eq 1 ]; then
        echo "ok $TOTAL - $1 # SKIP $2"
    else
        echo -e "${YELLOW}[SKIP]${NC} $1 - $2"
    fi
}

# Check if stradac exists
check_compiler() {
    if [ ! -x "$STRADAC" ]; then
        echo "Error: Strada compiler not found at $STRADAC"
        echo "Run 'make' first to build the compiler."
        exit 1
    fi
}

# Compile a Strada file
# Returns: 0 on success, 1 on failure
compile_strada() {
    local src="$1"
    local name="$2"
    local c_file="$BUILD_DIR/${name}.c"
    local exe_file="$BUILD_DIR/${name}"

    # Compile Strada to C (with memory limit to prevent OOM)
    if ! timeout 30 bash -c "ulimit -v $STRADAC_MEM_LIMIT_KB 2>/dev/null; exec \"\$@\"" _ "$STRADAC" "$src" "$c_file" > "$BUILD_DIR/${name}_strada.log" 2>&1; then
        return 1
    fi

    # Compile C to executable
    # Note: -rdynamic exports symbols so shared libraries can use the executable's runtime
    if ! gcc -rdynamic -o "$exe_file" "$c_file" "$RUNTIME" -I"$RUNTIME_H" -ldl -lm ${EXTRA_LDFLAGS:-} > "$BUILD_DIR/${name}_gcc.log" 2>&1; then
        return 2
    fi

    return 0
}

# Run a compiled program
# Returns: exit code of the program
run_program() {
    local name="$1"
    shift
    local exe_file="$BUILD_DIR/${name}"
    local timeout_secs="${1:-5}"

    timeout "$timeout_secs" "$exe_file" > "$BUILD_DIR/${name}.out" 2>&1
    return $?
}

# Compare output with expected
# Returns: 0 if match, 1 if different
compare_output() {
    local name="$1"
    local expected="$2"
    local actual="$BUILD_DIR/${name}.out"

    if [ -f "$expected" ]; then
        diff -q "$expected" "$actual" > /dev/null 2>&1
        return $?
    else
        # Expected is a string
        echo "$expected" | diff -q - "$actual" > /dev/null 2>&1
        return $?
    fi
}

# ============================================================
# Test Types
# ============================================================

# Test: Compile only (no execution)
test_compile() {
    local src="$1"
    local name="$2"
    local desc="${3:-$name}"

    TOTAL=$((TOTAL + 1))

    if compile_strada "$src" "$name"; then
        PASSED=$((PASSED + 1))
        log_pass "compile: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "compile: $desc" "$err"
        return 1
    fi
}

# Test: Compile and run (expect exit 0)
test_run() {
    local src="$1"
    local name="$2"
    local desc="${3:-$name}"
    local timeout="${4:-5}"

    TOTAL=$((TOTAL + 1))

    if ! compile_strada "$src" "$name"; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Compile failed: $err"
        return 1
    fi

    if run_program "$name" "$timeout"; then
        PASSED=$((PASSED + 1))
        log_pass "run: $desc"
        return 0
    else
        local exit_code=$?
        FAILED=$((FAILED + 1))
        if [ $exit_code -eq 124 ]; then
            log_fail "run: $desc" "Timeout after ${timeout}s"
        else
            log_fail "run: $desc" "Exit code: $exit_code"
        fi
        return 1
    fi
}

# Test: Compile, run, and check output
test_output() {
    local src="$1"
    local name="$2"
    local expected="$3"
    local desc="${4:-$name}"
    local timeout="${5:-5}"

    TOTAL=$((TOTAL + 1))

    if ! compile_strada "$src" "$name"; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "output: $desc" "Compile failed: $err"
        return 1
    fi

    run_program "$name" "$timeout"
    local exit_code=$?

    if [ $exit_code -eq 124 ]; then
        FAILED=$((FAILED + 1))
        log_fail "output: $desc" "Timeout after ${timeout}s"
        return 1
    fi

    if compare_output "$name" "$expected"; then
        PASSED=$((PASSED + 1))
        log_pass "output: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "output: $desc" "Output mismatch"
        if [ $VERBOSE -eq 1 ]; then
            echo "Expected:"
            echo "$expected" | head -5
            echo "Got:"
            cat "$BUILD_DIR/${name}.out" | head -5
        fi
        return 1
    fi
}

# Test: Check specific exit code
test_exit_code() {
    local src="$1"
    local name="$2"
    local expected_code="$3"
    local desc="${4:-$name}"

    TOTAL=$((TOTAL + 1))

    if ! compile_strada "$src" "$name"; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "exit: $desc" "Compile failed: $err"
        return 1
    fi

    run_program "$name" 5
    local actual_code=$?

    if [ $actual_code -eq $expected_code ]; then
        PASSED=$((PASSED + 1))
        log_pass "exit: $desc (code=$expected_code)"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "exit: $desc" "Expected exit $expected_code, got $actual_code"
        return 1
    fi
}

# Test: Check output contains pattern
test_output_contains() {
    local src="$1"
    local name="$2"
    local pattern="$3"
    local desc="${4:-$name}"

    TOTAL=$((TOTAL + 1))

    if ! compile_strada "$src" "$name"; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "contains: $desc" "Compile failed: $err"
        return 1
    fi

    run_program "$name" 5

    if grep -q "$pattern" "$BUILD_DIR/${name}.out" 2>/dev/null; then
        PASSED=$((PASSED + 1))
        log_pass "contains: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "contains: $desc" "Pattern not found: $pattern"
        return 1
    fi
}

# Test: Check that program fails with expected stderr pattern (for exception tests)
test_exception_output() {
    local src="$1"
    local name="$2"
    local pattern="$3"
    local desc="${4:-$name}"

    TOTAL=$((TOTAL + 1))

    if ! compile_strada "$src" "$name"; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "exception: $desc" "Compile failed: $err"
        return 1
    fi

    run_program "$name" 5
    local exit_code=$?

    # Check that the program exited with non-zero (exception was thrown)
    if [ $exit_code -eq 0 ]; then
        FAILED=$((FAILED + 1))
        log_fail "exception: $desc" "Expected non-zero exit, got 0"
        return 1
    fi

    # Check that output contains the expected pattern
    if grep -q "$pattern" "$BUILD_DIR/${name}.out" 2>/dev/null; then
        PASSED=$((PASSED + 1))
        log_pass "exception: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "exception: $desc" "Pattern not found: $pattern"
        if [ $VERBOSE -eq 1 ]; then
            echo "  Actual output:"
            cat "$BUILD_DIR/${name}.out" 2>/dev/null | head -20 | sed 's/^/    /'
        fi
        return 1
    fi
}

# Skip a test
test_skip() {
    local name="$1"
    local reason="$2"

    TOTAL=$((TOTAL + 1))
    SKIPPED=$((SKIPPED + 1))
    log_skip "$name" "$reason"
}

# Test: import_lib test (requires compiling a library to .so first)
test_import_lib() {
    local src="$1"
    local name="$2"
    local lib_src="$3"
    local lib_name="$4"
    local desc="${5:-$name}"
    local timeout_secs="${6:-5}"

    TOTAL=$((TOTAL + 1))

    # First compile the library to .so file in examples dir
    local lib_c="$EXAMPLES_DIR/${lib_name}.c"
    local lib_so="$EXAMPLES_DIR/${lib_name}.so"

    # Compile library source to C
    if ! timeout 30 bash -c "ulimit -v $STRADAC_MEM_LIMIT_KB 2>/dev/null; exec \"\$@\"" _ "$STRADAC" "$lib_src" "$lib_c" > "$BUILD_DIR/${lib_name}_strada.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Library compile failed: $err"
        return 1
    fi

    # Compile library C to .so
    # Note: Don't include runtime source - the library uses the executable's runtime symbols
    if ! gcc -shared -fPIC "$lib_c" -o "$lib_so" -I"$RUNTIME_H" -ldl -lm > "$BUILD_DIR/${lib_name}_gcc.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_gcc.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Library .so compile failed: $err"
        return 1
    fi

    # Create examples directory in BUILD_DIR and symlink the .so file
    # This ensures the relative path "examples/LibName.so" works when running from BUILD_DIR
    mkdir -p "$BUILD_DIR/examples"
    ln -sf "$lib_so" "$BUILD_DIR/examples/${lib_name}.so"

    # Now compile and run the test
    if ! compile_strada "$src" "$name"; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Compile failed: $err"
        return 1
    fi

    run_program "$name" "$timeout_secs"
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "run: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "run: $desc" "Exit code: $exit_code"
        if [ $VERBOSE -eq 1 ]; then
            cat "$BUILD_DIR/${name}.out" | head -20
        fi
        return 1
    fi
}

# Test: import_object test (requires compiling a library to .o first)
test_import_object() {
    local src="$1"
    local name="$2"
    local lib_src="$3"
    local lib_name="$4"
    local desc="${5:-$name}"
    local timeout_secs="${6:-5}"

    TOTAL=$((TOTAL + 1))

    # First compile the library to .o file in examples dir
    local lib_c="$EXAMPLES_DIR/${lib_name}.c"
    local lib_o="$EXAMPLES_DIR/${lib_name}.o"

    # Compile library source to C
    if ! timeout 30 bash -c "ulimit -v $STRADAC_MEM_LIMIT_KB 2>/dev/null; exec \"\$@\"" _ "$STRADAC" "$lib_src" "$lib_c" > "$BUILD_DIR/${lib_name}_strada.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Library compile failed: $err"
        return 1
    fi

    # Compile library C to .o
    if ! gcc -c -fPIC "$lib_c" -o "$lib_o" -I"$RUNTIME_H" > "$BUILD_DIR/${lib_name}_gcc.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_gcc.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Library .o compile failed: $err"
        return 1
    fi

    # Compile test source to C
    local c_file="$BUILD_DIR/${name}.c"
    local exe_file="$BUILD_DIR/${name}"

    if ! timeout 30 bash -c "ulimit -v $STRADAC_MEM_LIMIT_KB 2>/dev/null; exec \"\$@\"" _ "$STRADAC" "$src" "$c_file" > "$BUILD_DIR/${name}_strada.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Compile failed: $err"
        return 1
    fi

    # Compile C to executable - INCLUDE the .o file
    if ! gcc -o "$exe_file" "$c_file" "$lib_o" "$RUNTIME" -I"$RUNTIME_H" -ldl -lm ${EXTRA_LDFLAGS:-} > "$BUILD_DIR/${name}_gcc.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_gcc.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "GCC compile failed: $err"
        return 1
    fi

    run_program "$name" "$timeout_secs"
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "run: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "run: $desc" "Exit code: $exit_code"
        if [ $VERBOSE -eq 1 ]; then
            cat "$BUILD_DIR/${name}.out" | head -20
        fi
        return 1
    fi
}

# Test: import_archive test (requires compiling a library to .a first)
test_import_archive() {
    local src="$1"
    local name="$2"
    local lib_src="$3"
    local lib_name="$4"
    local desc="${5:-$name}"
    local timeout_secs="${6:-5}"

    TOTAL=$((TOTAL + 1))

    # First compile the library to .a file in examples dir
    local lib_c="$EXAMPLES_DIR/${lib_name}.c"
    local lib_o="$EXAMPLES_DIR/${lib_name}.o"
    local lib_runtime_o="$EXAMPLES_DIR/${lib_name}_runtime.o"
    local lib_a="$EXAMPLES_DIR/${lib_name}.a"

    # Compile library source to C
    if ! timeout 30 bash -c "ulimit -v $STRADAC_MEM_LIMIT_KB 2>/dev/null; exec \"\$@\"" _ "$STRADAC" "$lib_src" "$lib_c" > "$BUILD_DIR/${lib_name}_strada.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Library compile failed: $err"
        return 1
    fi

    # Compile library C to .o
    if ! gcc -c -fPIC "$lib_c" -o "$lib_o" -I"$RUNTIME_H" > "$BUILD_DIR/${lib_name}_gcc.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_gcc.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Library .o compile failed: $err"
        return 1
    fi

    # Use pre-built runtime .o (may have PCRE2 embedded via --static-pcre2)
    cp "$RUNTIME" "$lib_runtime_o"

    # Create archive
    if ! ar rcs "$lib_a" "$lib_o" "$lib_runtime_o" > "$BUILD_DIR/${lib_name}_ar.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${lib_name}_ar.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Archive creation failed: $err"
        return 1
    fi

    # Compile test source to C
    local c_file="$BUILD_DIR/${name}.c"
    local exe_file="$BUILD_DIR/${name}"

    if ! timeout 30 bash -c "ulimit -v $STRADAC_MEM_LIMIT_KB 2>/dev/null; exec \"\$@\"" _ "$STRADAC" "$src" "$c_file" > "$BUILD_DIR/${name}_strada.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_strada.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "Compile failed: $err"
        return 1
    fi

    # Compile C to executable - archive includes runtime, so skip separate runtime
    if ! gcc -rdynamic -o "$exe_file" "$c_file" "$lib_a" -I"$RUNTIME_H" -ldl -lm -lpthread ${EXTRA_LDFLAGS:-} > "$BUILD_DIR/${name}_gcc.log" 2>&1; then
        FAILED=$((FAILED + 1))
        local err=$(cat "$BUILD_DIR/${name}_gcc.log" 2>/dev/null | head -1)
        log_fail "run: $desc" "GCC compile failed: $err"
        return 1
    fi

    run_program "$name" "$timeout_secs"
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "run: $desc"
        return 0
    else
        FAILED=$((FAILED + 1))
        log_fail "run: $desc" "Exit code: $exit_code"
        if [ $VERBOSE -eq 1 ]; then
            cat "$BUILD_DIR/${name}.out" | head -20
        fi
        return 1
    fi
}

# ============================================================
# Main Test Execution
# ============================================================

check_compiler

if [ $TAP_FORMAT -eq 1 ]; then
    echo "TAP version 13"
fi

log_info "Starting Strada test suite"
log_info "Compiler: $STRADAC"
log_info "Build dir: $BUILD_DIR"

# Source test definitions
for test_file in "$SCRIPT_DIR"/t_*.sh; do
    if [ -f "$test_file" ]; then
        if [ -z "$TEST_PATTERN" ] || [[ "$(basename "$test_file")" == *"$TEST_PATTERN"* ]]; then
            source "$test_file"
        fi
    fi
done

# If no test files found, run default tests on examples
if [ $TOTAL -eq 0 ]; then
    log_info "Running compile tests on all examples..."

    for src in "$EXAMPLES_DIR"/*.strada; do
        name=$(basename "$src" .strada)

        # Skip if pattern doesn't match
        if [ -n "$TEST_PATTERN" ] && [[ "$name" != *"$TEST_PATTERN"* ]]; then
            continue
        fi

        # Skip server programs (they run indefinitely)
        case "$name" in
            prefork_server|simple_select_server|web_server|web_server_select|test_socket_server)
                test_skip "$name" "Server program (runs indefinitely)"
                continue
                ;;
        esac

        if [ $COMPILE_ONLY -eq 1 ]; then
            test_compile "$src" "$name"
        else
            test_run "$src" "$name" "$name" 3
        fi
    done
fi

# Print summary
echo ""
if [ $TAP_FORMAT -eq 1 ]; then
    echo "1..$TOTAL"
else
    echo "========================================"
    echo -e "Tests: $TOTAL  ${GREEN}Passed: $PASSED${NC}  ${RED}Failed: $FAILED${NC}  ${YELLOW}Skipped: $SKIPPED${NC}"
    echo "========================================"
fi

# Exit with failure if any tests failed
if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
