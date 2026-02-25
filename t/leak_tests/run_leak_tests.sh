#!/bin/bash
# Run all leak tests with valgrind
# Usage: ./run_leak_tests.sh

set +e  # Errors handled per-test below

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
    "test_regex"
    "test_fileio"
    "test_weakrefs"
    "test_refs"
    "test_functional"
    "test_splice"
    "test_dispatch"
    "test_overload"
    "test_pack"
    "test_destructure"
    "test_stringbuilder"
    "test_globals"
    "test_time"
    "test_filesystem"
    "test_process"
    "test_misc"
    "test_math_advanced"
    "test_utf8"
    "test_string_ops"
    "test_range"
    "test_closures_advanced"
    "test_oop_advanced"
    "test_regex_advanced"
    "test_scope_cleanup"
    "test_slices"
    "test_operators"
    "test_error_handling"
    "test_tie"
    "test_clone"
    "test_hash_each"
    "test_base64_hex"
    "test_local_dynamic"
    "test_sprintf_format"
    "test_signals"
    "test_hash_access_cleanup"
    "test_hash_arg_patterns"
    "test_builtin_hash_args"
    "test_sys_hash_args"
    "test_hash_list_init"
    "test_hash_pool"
    "test_small_int_pool"
    "test_regex_cache"
    "test_method_cache"
    "test_str_buf"
    "test_intern_strings"
    "test_array_shift_opt"
    "test_metadata_pool"
    "test_weak_registry_opt"
    "test_str_buf_syscalls"
    "test_str_eq_shortcircuit"
    "test_concat_inplace_growth"
    "test_split_single_alloc"
    "test_intern_resize"
    "test_sv_pool_large"
    "test_default_capacities"
    "test_meta_pool_bless"
    "test_perf_opts_round2"
    "test_oop_fast_ctor"
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
