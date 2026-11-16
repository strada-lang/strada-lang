#!/bin/bash
# Run all leak tests with valgrind
# Usage: ./run_leak_tests.sh [options] [filter]
#   --default, --default-opt   Use compiler default optimization params (instead of -O0)
#   -O LEVEL                   Use -OLEVEL (e.g. -O0, -O2)
#   -j N, --jobs N             Parallel build+valgrind jobs (default: nproc)
#   -v, --verbose              Show full per-test output even on pass
#   -h, --help                 Show this help

set +e  # Errors handled per-test below

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STRADA_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$STRADA_DIR"

DEFAULT_OPT=0
OPT_LEVEL="0"
JOBS="${STRADA_LEAK_JOBS:-$(nproc 2>/dev/null || echo 4)}"
VERBOSE=0
FILTER=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --default|--default-opt)
            DEFAULT_OPT=1
            shift
            ;;
        -O)
            if [[ $# -lt 2 ]]; then echo "Option -O requires an argument" >&2; exit 2; fi
            OPT_LEVEL="$2"
            DEFAULT_OPT=0
            shift 2
            ;;
        -O*)
            OPT_LEVEL="${1#-O}"
            DEFAULT_OPT=0
            shift
            ;;
        -j|--jobs)
            if [[ $# -lt 2 ]]; then echo "Option $1 requires an argument" >&2; exit 2; fi
            JOBS="$2"
            shift 2
            ;;
        -j*)
            JOBS="${1#-j}"
            shift
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -h|--help)
            head -9 "$0" | tail -8
            exit 0
            ;;
        *)
            FILTER="$1"
            shift
            ;;
    esac
done

if [[ -n "${STRADA_LEAK_DEFAULT_OPT:-}" && "${STRADA_LEAK_DEFAULT_OPT}" != "0" ]]; then
    DEFAULT_OPT=1
fi

if [[ "$DEFAULT_OPT" -eq 1 ]]; then
    LEAK_OPT_FLAG=""
else
    LEAK_OPT_FLAG="-O${OPT_LEVEL}"
fi

# Tests that currently pass without memory leaks
TESTS=(
    "test_digest_sha"
    "test_digest_md5"
    "test_crypt"
    "test_random"
    "test_combined"
    "test_strings"
    "test_utf8_index_substr"
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
    "test_our_container_assign"
    "test_time"
    "test_filesystem"
    "test_process"
    "test_misc"
    "test_math_advanced"
    "test_utf8"
    "test_utf8_ops"
    "test_vec_ops"
    "test_local_hash_slice"
    "test_string_ops"
    "test_range"
    "test_closures_advanced"
    "test_oop_advanced"
    "test_regex_advanced"
    "test_scope_cleanup"
    "test_ref_to_temp_and_deref_assign"
    "test_hash_literal_keys"
    "test_borrowed_in_nested_literals"
    "test_lexical_globals"
    "test_deref_temp_and_hash_copy"
    "test_foreach_early_exit_cleanup"
    "test_builtin_temp_args"
    "test_str_builtin_temp_keys"
    "test_method_throw_temp_args"
    "test_throw_after_reassign"
    "test_main_params"
    "test_foreach_var_write"
    "test_socket_owned_args"
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
    "test_hash_expr_keys"
    "test_literal_subst"
    "test_oop_fast_accessor"
    "test_compound_assign"
    "test_concat_hash_keys"
    "test_in_memory_io"
    "test_byte_and_pwgr_hash_args"
    "test_switch_inner_scope"
    "test_delete_returns_value"
    "test_dyn_method_call_hash_recv"
    "test_inline_ctor_var_args"
    "test_concat_self_alias"
    "test_async_basic_leak"
    "test_opendir_no_explicit_close"
    "test_nested_hash_subscript_leaks"
    "test_sort_hash_leak"
    "test_regex_g_named_captures"
    "test_regex_capture_leaks"
    "test_hash_store_owned_values"
    "test_subscript_owned_source"
    "test_method_spread_leak"
    "test_assign_goto_redo_leaks"
    "test_exists_delete_compound_leaks"
    "test_method_throw_leak"
    "test_throw_paths_round10"
    "test_throw_paths_round11"
    "test_round12_hashinc_overload_hooks"
    "test_round13_sprintf_anon_mod"
    "test_round14_closures_async"
    "test_round15_grep_async_mutex"
    "test_round16_local_method_hash"
    "test_round17_local_loop_profile"
    "test_socket_close_inline"
    "test_socket_lifecycle"
    "test_round18_runtime_internals"
    "test_round19_architectural"
    "test_cycle_collector"
    "test_threads_cycles"
    "test_threads_gc_race"
    "test_arena"
    "test_gc_arena_api"
    "test_bless_idioms"
    "test_pkg_unqualified_call"
    "test_strada_extern_decl"
    "test_hash_set_value_leak"
    "test_async_ergonomics"
    "test_import_lib_devirt"
    "test_arena_hardened"
    "test_encoding"
    "test_audit_2026_11_leaks"
    "test_fast_codegen_leaks"
    "test_closure_cycles_and_tls"
    "test_phase3_codegen_leaks"
    "test_phase4_stdlib_leaks"
    "test_phase5_eval_jit_leaks"
    "test_audit_r2_leaks"
    "test_audit_r3_leaks"
    "test_audit_r4_leaks"
    "test_audit_r5_leaks"
    "test_audit_r5_eval_jit_leaks"
    "test_audit_r5_stdlib_leaks"
    "test_audit_r6_leaks"
    "test_audit_r6_eval_jit_leaks"
    "test_audit_r7_leaks"
    "test_audit_r7_eval_jit_leaks"
    "test_audit_r8_leaks"
    "test_audit_r8_eval_jit_leaks"
    "test_audit_r9_leaks"
    "test_audit_r9_eval_jit_leaks"
    "test_audit_r10_leaks"
    "test_audit_r10_eval_jit_leaks"
    "test_audit_r11_leaks"
    "test_audit_r11_eval_jit_leaks"
    "test_audit_r12_leaks"
    "test_audit_r12_eval_jit_leaks"
    "test_audit_r13_leaks"
    "test_audit_r13_eval_jit_leaks"
    "test_audit_r14_leaks"
    "test_audit_r14_eval_jit_leaks"
    "test_audit_r15_leaks"
    "test_audit_r15_eval_jit_leaks"
    "test_audit_r16_leaks"
    "test_audit_r16_eval_jit_leaks"
    "test_audit_r17_leaks"
    "test_audit_r17_eval_jit_leaks"
    "test_mem_simplify"
)

# c::callback needs libffi at build time — include only when configured in
# (the exit-time trampoline registry sweep is what keeps this leak-free).
if grep -q "^export STRADA_HAVE_LIBFFI=1" "$STRADA_DIR/config.sh" 2>/dev/null; then
    TESTS+=("test_c_callback")
fi

if [ -f "$STRADA_DIR/config.sh" ]; then
    . "$STRADA_DIR/config.sh"
fi

PASSED=0
FAILED=0

TMP_LOG_DIR=$(mktemp -d)
cleanup_leak_runner() {
    rm -rf "$TMP_LOG_DIR"
    for t in "${TESTS[@]}"; do
        rm -f "t/leak_tests/${t}"
    done
    rm -f "t/leak_tests/test_import_lib_devirt_lib.so"
}
trap cleanup_leak_runner EXIT

SELECTED=()
for test in "${TESTS[@]}"; do
    if [[ -n "$FILTER" ]] && [[ "$test" != *"$FILTER"* ]]; then
        continue
    fi
    SELECTED+=("$test")
done

TOTAL_SEL=${#SELECTED[@]}
echo "========================================"
echo "Running Leak Tests ($TOTAL_SEL tests, opt=${LEAK_OPT_FLAG:-default}, jobs=$JOBS)"
echo "========================================"
echo ""

run_one_leak() {
    local test="$1"
    local out_file="$TMP_LOG_DIR/${test}.log"
    local rc_file="$TMP_LOG_DIR/${test}.rc"

    local EXTRA_FLAGS=""
    case "$test" in
        test_crypt|test_combined)
            EXTRA_FLAGS="-lcrypt"
            ;;
        test_audit_r3_leaks)
            EXTRA_FLAGS="-lcrypt -lsqlite3"
            ;;
        test_audit_r5_stdlib_leaks)
            EXTRA_FLAGS="-lsqlite3"
            ;;
        test_audit_r7_leaks|test_audit_r8_leaks|test_audit_r9_leaks|test_audit_r11_leaks|test_audit_r12_leaks)
            EXTRA_FLAGS="-lpthread"
            ;;
        test_audit_r13_leaks|test_audit_r14_leaks|test_audit_r15_leaks|test_audit_r16_leaks|test_audit_r17_leaks)
            if [ -f "$STRADA_DIR/config.sh" ]; then
                EXTRA_FLAGS="-L lib ${STRADA_ZLIB_LIBS:--lz} ${STRADA_DBI_LIBS:-} -lpthread"
            else
                EXTRA_FLAGS="-L lib -lz -lsqlite3 -lpthread"
            fi
            ;;
        test_audit_r5_eval_jit_leaks|test_audit_r6_eval_jit_leaks|test_audit_r7_eval_jit_leaks|test_audit_r8_eval_jit_leaks|test_audit_r9_eval_jit_leaks|test_audit_r10_eval_jit_leaks|test_audit_r11_eval_jit_leaks|test_audit_r12_eval_jit_leaks|test_audit_r13_eval_jit_leaks|test_audit_r14_eval_jit_leaks|test_audit_r15_eval_jit_leaks|test_audit_r16_eval_jit_leaks|test_audit_r17_eval_jit_leaks)
            EXTRA_FLAGS="--no-use-artifacts -L lib"
            ;;
        test_import_lib_devirt)
            if ! ./strada $LEAK_OPT_FLAG ${STRADA_LEAK_FLAGS:-} --shared "t/leak_tests/test_import_lib_devirt_lib.strada" \
                    -o "t/leak_tests/test_import_lib_devirt_lib.so" > "$out_file" 2>&1; then
                {
                    echo "----------------------------------------"
                    echo "FAIL: $test (companion .so build failed)"
                    cat "$out_file"
                } > "${out_file}.print"
                mv "${out_file}.print" "$out_file"
                echo 1 > "$rc_file"
                return
            fi
            ;;
    esac

    local build_out
    if ! build_out=$(./strada $LEAK_OPT_FLAG ${STRADA_LEAK_FLAGS:-} "t/leak_tests/${test}.strada" -o "t/leak_tests/${test}" $EXTRA_FLAGS 2>&1); then
        {
            echo "----------------------------------------"
            echo "FAIL: $test (compilation failed)"
            echo "$build_out"
        } > "$out_file"
        echo 1 > "$rc_file"
        return
    fi

    local VALGRIND_OUT VALGRIND_EXIT DEFINITELY_LOST INDIRECTLY_LOST
    VALGRIND_OUT=$(valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99 "t/leak_tests/${test}" 2>&1)
    VALGRIND_EXIT=$?
    DEFINITELY_LOST=$(echo "$VALGRIND_OUT" | grep "definitely lost:" | awk '{print $4}')
    INDIRECTLY_LOST=$(echo "$VALGRIND_OUT" | grep "indirectly lost:" | awk '{print $4}')

    if [[ "$VALGRIND_EXIT" -eq 0 ]] && [[ "$DEFINITELY_LOST" == "0" ]] && [[ "$INDIRECTLY_LOST" == "0" ]]; then
        if [[ "$VERBOSE" -eq 1 ]]; then
            {
                echo "----------------------------------------"
                echo "Building: $test"
                echo "----------------------------------------"
                [ -n "$build_out" ] && echo "$build_out"
                echo ""
                echo "Running with valgrind..."
                echo "$VALGRIND_OUT" | grep -v "^==" || true
                echo "LEAK CHECK: PASS (no leaks detected)"
                echo ""
            } > "$out_file"
        else
            echo "PASS: $test" > "$out_file"
        fi
        echo 0 > "$rc_file"
    else
        {
            echo "----------------------------------------"
            echo "LEAK CHECK: FAIL: $test (exit=$VALGRIND_EXIT)"
            echo "  Definitely lost: $DEFINITELY_LOST bytes"
            echo "  Indirectly lost: $INDIRECTLY_LOST bytes"
            [ -n "$build_out" ] && echo "$build_out"
            echo "$VALGRIND_OUT" | grep -v "^==" || true
            echo "Valgrind summary:"
            echo "$VALGRIND_OUT" | grep -A5 "LEAK SUMMARY"
            echo ""
        } > "$out_file"
        echo 1 > "$rc_file"
    fi
}

PIDS=()
PNAMES=()
for test in "${SELECTED[@]}"; do
    if [[ "${JOBS:-1}" -le 1 ]]; then
        run_one_leak "$test"
        cat "$TMP_LOG_DIR/${test}.log"
        if [[ "$(cat "$TMP_LOG_DIR/${test}.rc")" -eq 0 ]]; then
            PASSED=$((PASSED + 1))
        else
            FAILED=$((FAILED + 1))
        fi
    else
        run_one_leak "$test" &
        PIDS+=("$!")
        PNAMES+=("$test")
        if [[ "${#PIDS[@]}" -ge "$JOBS" ]]; then
            wait "${PIDS[0]}"
            n0="${PNAMES[0]}"
            cat "$TMP_LOG_DIR/${n0}.log"
            if [[ "$(cat "$TMP_LOG_DIR/${n0}.rc")" -eq 0 ]]; then
                PASSED=$((PASSED + 1))
            else
                FAILED=$((FAILED + 1))
            fi
            PIDS=("${PIDS[@]:1}")
            PNAMES=("${PNAMES[@]:1}")
        fi
    fi
done

for idx in "${!PIDS[@]}"; do
    wait "${PIDS[$idx]}"
    n="${PNAMES[$idx]}"
    cat "$TMP_LOG_DIR/${n}.log"
    if [[ "$(cat "$TMP_LOG_DIR/${n}.rc")" -eq 0 ]]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "========================================"
echo "Summary: $PASSED passed, $FAILED failed"
echo "========================================"

if [[ $FAILED -gt 0 ]]; then
    exit 1
fi
exit 0
