#!/bin/bash
# Run the compiler test suite source files through the interpreter
# to find language gaps between compiled and interpreted execution.
#
# Usage: bash run_compiler_tests.sh [-v]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INTERP="$SCRIPT_DIR/../strada-interp"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXAMPLES_DIR="$PROJECT_DIR/examples"
T_DIR="$PROJECT_DIR/t"

VERBOSE=0
if [ "$1" = "-v" ]; then
    VERBOSE=1
fi

PASS=0
FAIL=0
SKIP=0
TOTAL=0
ERRORS=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

run_test() {
    local src="$1"
    local name="$2"
    local desc="$3"
    local mode="$4"        # "run", "output", "contains", "exit_code"
    local expected="$5"
    local timeout_secs="${6:-5}"

    TOTAL=$((TOTAL + 1))

    # Run through interpreter with timeout
    local output
    output=$(timeout "$timeout_secs" "$INTERP" "$src" 2>&1)
    local exit_code=$?

    case "$mode" in
        run)
            # Just check exit code is 0
            if [ $exit_code -eq 0 ]; then
                PASS=$((PASS + 1))
                echo -e "  ${GREEN}PASS${NC}: $desc"
            elif [ $exit_code -eq 124 ]; then
                FAIL=$((FAIL + 1))
                echo -e "  ${RED}FAIL${NC}: $desc (timeout)"
                ERRORS="$ERRORS\n  $desc: TIMEOUT"
            else
                FAIL=$((FAIL + 1))
                echo -e "  ${RED}FAIL${NC}: $desc (exit $exit_code)"
                ERRORS="$ERRORS\n  $desc: exit $exit_code"
                if [ $VERBOSE -eq 1 ]; then
                    echo "$output" | head -5 | sed 's/^/    /'
                fi
            fi
            ;;
        output)
            # Check exact output match
            if [ $exit_code -ne 0 ] && [ $exit_code -ne 124 ]; then
                FAIL=$((FAIL + 1))
                echo -e "  ${RED}FAIL${NC}: $desc (exit $exit_code)"
                ERRORS="$ERRORS\n  $desc: exit $exit_code"
                if [ $VERBOSE -eq 1 ]; then
                    echo "$output" | head -5 | sed 's/^/    /'
                fi
                return
            fi
            if [ "$output" = "$expected" ]; then
                PASS=$((PASS + 1))
                echo -e "  ${GREEN}PASS${NC}: $desc"
            else
                FAIL=$((FAIL + 1))
                echo -e "  ${RED}FAIL${NC}: $desc (output mismatch)"
                ERRORS="$ERRORS\n  $desc: output mismatch"
                if [ $VERBOSE -eq 1 ]; then
                    echo "    Expected: $(echo "$expected" | head -3)"
                    echo "    Got:      $(echo "$output" | head -3)"
                fi
            fi
            ;;
        contains)
            # Check output contains pattern
            if echo "$output" | grep -q "$expected" 2>/dev/null; then
                PASS=$((PASS + 1))
                echo -e "  ${GREEN}PASS${NC}: $desc"
            else
                FAIL=$((FAIL + 1))
                echo -e "  ${RED}FAIL${NC}: $desc (pattern not found: $expected)"
                ERRORS="$ERRORS\n  $desc: pattern '$expected' not found"
                if [ $VERBOSE -eq 1 ]; then
                    echo "$output" | head -5 | sed 's/^/    /'
                fi
            fi
            ;;
        exit_code)
            if [ $exit_code -eq "$expected" ]; then
                PASS=$((PASS + 1))
                echo -e "  ${GREEN}PASS${NC}: $desc (exit=$expected)"
            else
                FAIL=$((FAIL + 1))
                echo -e "  ${RED}FAIL${NC}: $desc (expected exit $expected, got $exit_code)"
                ERRORS="$ERRORS\n  $desc: expected exit $expected, got $exit_code"
            fi
            ;;
    esac
}

skip_test() {
    local desc="$1"
    local reason="$2"
    TOTAL=$((TOTAL + 1))
    SKIP=$((SKIP + 1))
    echo -e "  ${YELLOW}SKIP${NC}: $desc ($reason)"
}

echo "=== Running Compiler Tests on Interpreter ==="
echo "Interpreter: $INTERP"
echo ""

# ---- Core tests from t/t_core.sh ----

echo "--- Core Tests ---"

run_test "$EXAMPLES_DIR/test_simple.strada" "test_simple" "Basic arithmetic" "output" "30"
run_test "$EXAMPLES_DIR/test_exit.strada" "test_exit" "Exit success" "exit_code" "0"
run_test "$EXAMPLES_DIR/test_exit_error.strada" "test_exit_error" "Exit with error" "exit_code" "1"
run_test "$EXAMPLES_DIR/test_strings.strada" "test_strings" "String functions" "contains" "All tests passed"
run_test "$EXAMPLES_DIR/control_flow_demo.strada" "control_flow_demo" "Control flow" "run" ""
run_test "$EXAMPLES_DIR/test_control_flow2.strada" "test_control_flow2" "Control flow 2" "contains" "All control flow tests passed"
run_test "$EXAMPLES_DIR/array_ops.strada" "array_ops" "Array operations" "run" ""
run_test "$EXAMPLES_DIR/test_references.strada" "test_references" "References" "run" ""
run_test "$EXAMPLES_DIR/test_refs.strada" "test_refs" "Refs" "run" ""
run_test "$EXAMPLES_DIR/test_weak_ref.strada" "test_weak_ref" "Weak references" "contains" "All weak reference tests passed"
run_test "$EXAMPLES_DIR/test_native_hash.strada" "test_native_hash" "Native hash" "run" ""
run_test "$EXAMPLES_DIR/test_hash_barekeys.strada" "test_hash_barekeys" "Hash barekeys" "run" ""
run_test "$EXAMPLES_DIR/test_hash_refcount.strada" "test_hash_refcount" "Hash refcount" "contains" "All hash refcount tests passed"
run_test "$EXAMPLES_DIR/anon_array_refcount.strada" "anon_array_refcount" "Anon array refcount" "contains" "PASS: Anonymous array refcount test"
run_test "$EXAMPLES_DIR/temp_cleanup_test.strada" "temp_cleanup_test" "Temp cleanup" "contains" "PASS: Temporary cleanup test completed"
run_test "$EXAMPLES_DIR/test_param_reassign.strada" "test_param_reassign" "Param reassign" "contains" "All parameter reassignment tests passed"
run_test "$EXAMPLES_DIR/test_oop.strada" "test_oop" "OOP basics" "run" ""
run_test "$EXAMPLES_DIR/test_dyn_dispatch.strada" "test_dyn_dispatch" "Dynamic method dispatch" "contains" "All dynamic dispatch tests passed"
run_test "$EXAMPLES_DIR/test_packages.strada" "test_packages" "Packages" "run" ""
run_test "$EXAMPLES_DIR/test_regex.strada" "test_regex" "Regex" "run" ""
run_test "$EXAMPLES_DIR/test_pcre2.strada" "test_pcre2" "PCRE2 regex" "contains" "All PCRE2 tests passed"
run_test "$EXAMPLES_DIR/test_inline_regex.strada" "test_inline_regex" "Inline regex" "run" ""
run_test "$EXAMPLES_DIR/optional_params.strada" "optional_params" "Optional params" "run" ""
run_test "$EXAMPLES_DIR/optional_simple.strada" "optional_simple" "Optional simple" "run" ""
run_test "$EXAMPLES_DIR/variable_args_demo.strada" "variable_args_demo" "Variable args" "run" ""
run_test "$EXAMPLES_DIR/spread_operator.strada" "spread_operator" "Spread operator" "run" ""
run_test "$EXAMPLES_DIR/test_variadic.strada" "test_variadic" "Variadic functions" "run" ""
run_test "$EXAMPLES_DIR/test_variadic_oop.strada" "test_variadic_oop" "Variadic OOP methods" "run" ""
skip_test "import_lib variadic" "requires .so compilation"
skip_test "import_object variadic" "requires .o compilation"
run_test "$EXAMPLES_DIR/negative_index.strada" "negative_index" "Negative index" "run" ""
run_test "$EXAMPLES_DIR/test_try_catch.strada" "test_try_catch" "Try/catch" "run" ""
run_test "$T_DIR/test_exception_rethrow.strada" "test_exception_rethrow" "Exception re-throw" "contains" "PASS: All 5 exceptions caught"
skip_test "Stack traces" "interpreter stack traces differ from compiled"
run_test "$T_DIR/test_const.strada" "test_const" "Const declarations" "contains" "All const tests passed"
run_test "$T_DIR/test_recursion_limit.strada" "test_recursion_limit" "Recursion limit" "contains" "All recursion limit tests passed"
run_test "$EXAMPLES_DIR/test_goto.strada" "test_goto" "Goto" "run" ""
run_test "$EXAMPLES_DIR/test_loop_labels.strada" "test_loop_labels" "Loop labels" "run" ""
run_test "$EXAMPLES_DIR/test_do_while.strada" "test_do_while" "Do-while loops" "run" ""
run_test "$EXAMPLES_DIR/test_foreach.strada" "test_foreach" "Foreach loops" "run" ""
run_test "$EXAMPLES_DIR/test_json.strada" "test_json" "JSON" "run" ""
run_test "$EXAMPLES_DIR/test_sort.strada" "test_sort" "Sort" "run" ""
run_test "$EXAMPLES_DIR/test_map_sort.strada" "test_map_sort" "Map sort" "run" ""
run_test "$EXAMPLES_DIR/test_magic_constants.strada" "test_magic_constants" "Magic constants" "run" ""
run_test "$EXAMPLES_DIR/test_universal_isa.strada" "test_universal_isa" "ISA method" "run" ""
run_test "$EXAMPLES_DIR/test_universal_can.strada" "test_universal_can" "CAN method" "run" ""
run_test "$EXAMPLES_DIR/dumper_demo.strada" "dumper_demo" "Dumper demo" "run" ""
run_test "$EXAMPLES_DIR/test_free.strada" "test_free" "Memory free" "run" ""
run_test "$EXAMPLES_DIR/test_file_write.strada" "test_file_write" "File write" "run" ""
run_test "$EXAMPLES_DIR/test_process.strada" "test_process" "Process ops" "run" ""
run_test "$EXAMPLES_DIR/test_system.strada" "test_system" "System calls" "run" ""
run_test "$EXAMPLES_DIR/test_posix.strada" "test_posix" "POSIX functions" "run" ""
skip_test "Signal handling" "compile-only test"
skip_test "C shared library" "uses __C__ blocks with C types"
run_test "$EXAMPLES_DIR/test_math.strada" "test_math" "Math functions" "run" ""
run_test "$EXAMPLES_DIR/test_directory.strada" "test_directory" "Directory functions" "run" ""
run_test "$EXAMPLES_DIR/test_path.strada" "test_path" "Path functions" "run" ""
run_test "$EXAMPLES_DIR/test_seek.strada" "test_seek" "File seek functions" "run" ""
run_test "$EXAMPLES_DIR/test_dns.strada" "test_dns" "DNS functions" "run" ""
run_test "$EXAMPLES_DIR/test_math_libc.strada" "test_math_libc" "Extended math" "contains" "All math libc tests passed"
run_test "$EXAMPLES_DIR/test_process_libc.strada" "test_process_libc" "Extended process" "contains" "All process libc tests passed"
run_test "$EXAMPLES_DIR/test_user_libc.strada" "test_user_libc" "User/group" "contains" "All user libc tests passed"
run_test "$EXAMPLES_DIR/test_fileio_libc.strada" "test_fileio_libc" "File I/O" "contains" "All file I/O libc tests passed"
run_test "$EXAMPLES_DIR/test_socket_libc.strada" "test_socket_libc" "Socket" "contains" "All socket libc tests passed"
run_test "$EXAMPLES_DIR/test_misc_libc.strada" "test_misc_libc" "Misc libc" "contains" "All misc libc tests passed"
skip_test "Closures (compile only)" "compile-only test"
run_test "$EXAMPLES_DIR/test_closure_params.strada" "test_closure_params" "Closure params" "contains" "All closure parameter tests passed"
run_test "$EXAMPLES_DIR/test_operators.strada" "test_operators" "Operators" "run" ""
run_test "$EXAMPLES_DIR/test_switch.strada" "test_switch" "Switch statement" "run" ""
run_test "$EXAMPLES_DIR/test_ternary.strada" "test_ternary" "Ternary operator" "run" ""
run_test "$EXAMPLES_DIR/test_range.strada" "test_range" "Range operator" "run" ""
run_test "$EXAMPLES_DIR/test_namespaces.strada" "test_namespaces" "Namespaces" "run" ""
run_test "$EXAMPLES_DIR/test_multi_inherit.strada" "test_multi_inherit" "Multiple inheritance" "run" ""
run_test "$EXAMPLES_DIR/test_oop2.strada" "test_oop2" "OOP extended" "run" ""
run_test "$EXAMPLES_DIR/test_package_full.strada" "test_package_full" "Package full" "run" ""
run_test "$EXAMPLES_DIR/test_bool_context.strada" "test_bool_context" "Boolean context" "run" ""
run_test "$EXAMPLES_DIR/test_interpolation.strada" "test_interpolation" "String interpolation" "run" ""
run_test "$EXAMPLES_DIR/test_slurp.strada" "test_slurp" "File slurp" "run" ""
run_test "$EXAMPLES_DIR/test_argv.strada" "test_argv" "ARGV handling" "run" ""
run_test "$EXAMPLES_DIR/text_csv_demo.strada" "text_csv_demo" "CSV parsing" "run" ""
run_test "$EXAMPLES_DIR/test_memory.strada" "test_memory" "Memory management" "run" ""
skip_test "ls command" "compile-only test"
skip_test "ps command" "compile-only test"
run_test "$EXAMPLES_DIR/test_lwp.strada" "test_lwp" "LWP HTTP library" "contains" "All LWP tests passed"
run_test "$EXAMPLES_DIR/test_datetime.strada" "test_datetime" "DateTime library" "contains" "All DateTime tests passed"
skip_test "Nesso ORM" "requires SQLite linking"
run_test "$T_DIR/test_nested_use.strada" "test_nested_use" "Nested use" "contains" "All nested use tests passed"
run_test "$T_DIR/test_nested_oop.strada" "test_nested_oop" "Nested OOP" "contains" "All nested OOP tests passed"
skip_test "import_lib OOP" "requires .so compilation"
skip_test "import_object OOP" "requires .o compilation"
skip_test "import_archive OOP" "requires .a compilation"
run_test "$EXAMPLES_DIR/test_moose.strada" "test_moose" "Moose-style OOP" "output" "Rex
3
4
100
80
dog (energy: 80)
[preparing to bark]
Rex barks!
[done barking]
1
1"
run_test "$EXAMPLES_DIR/test_dynamic.strada" "test_dynamic" "Dynamic return type" "contains" "ok"
run_test "$EXAMPLES_DIR/test_utf8_ns.strada" "test_utf8_ns" "UTF-8 namespace" "contains" "10 passed, 0 failed"
run_test "$EXAMPLES_DIR/test_slices.strada" "test_slices" "Array/hash slices" "contains" "All slice tests passed"
run_test "$EXAMPLES_DIR/test_our.strada" "test_our" "Our variables" "contains" "All our variable tests passed"
run_test "$EXAMPLES_DIR/test_begin_end.strada" "test_begin_end" "BEGIN/END blocks" "contains" "PASS: BEGIN/END blocks"
run_test "$EXAMPLES_DIR/test_autoload.strada" "test_autoload" "AUTOLOAD" "contains" "All AUTOLOAD tests passed"
run_test "$EXAMPLES_DIR/test_overload.strada" "test_overload" "Overload" "contains" "All overload tests passed"
run_test "$EXAMPLES_DIR/test_bigint.strada" "test_bigint" "BigInt" "contains" "All BigInt tests passed"
run_test "$EXAMPLES_DIR/test_bigfloat.strada" "test_bigfloat" "BigFloat" "contains" "All BigFloat tests passed"
run_test "$EXAMPLES_DIR/test_str_repeat.strada" "test_str_repeat" "String repeat" "contains" "All str repeat tests passed"
run_test "$EXAMPLES_DIR/test_splice.strada" "test_splice" "Splice" "contains" "All splice tests passed"
run_test "$EXAMPLES_DIR/test_each.strada" "test_each" "Each" "contains" "All each tests passed"
run_test "$EXAMPLES_DIR/test_select_fh.strada" "test_select_fh" "Select" "contains" "All select tests passed"
run_test "$EXAMPLES_DIR/test_tr.strada" "test_tr" "Transliteration" "contains" "All tr tests passed"
run_test "$EXAMPLES_DIR/test_local.strada" "test_local" "Local" "contains" "All local tests passed"
run_test "$EXAMPLES_DIR/test_regex_eval.strada" "test_regex_eval" "Regex eval" "contains" "All regex eval tests passed"
run_test "$EXAMPLES_DIR/test_tie.strada" "test_tie" "Tie" "contains" "All tie tests passed"

# ---- Compile-only and special tests from t/t_compile.sh and t/t_special.sh ----

echo ""
echo "--- Compile Tests (running through interpreter) ---"

# These are normally compile-only, but let's try running them
for src in "$EXAMPLES_DIR"/deref_expr.strada \
           "$EXAMPLES_DIR"/deref_set.strada \
           "$EXAMPLES_DIR"/test_deref_exprs.strada \
           "$EXAMPLES_DIR"/test_perl_refs.strada \
           "$EXAMPLES_DIR"/pass_by_ref.strada \
           "$EXAMPLES_DIR"/test_qq.strada \
           "$EXAMPLES_DIR"/test_extern_packages.strada \
           "$EXAMPLES_DIR"/test_modules.strada \
           "$EXAMPLES_DIR"/test_ffi.strada; do
    name=$(basename "$src" .strada)
    if [ -f "$src" ]; then
        run_test "$src" "$name" "$name" "run" "" 5
    fi
done

echo ""
echo "--- Special Tests ---"

for src in "$EXAMPLES_DIR"/test_oop_improved.strada \
           "$EXAMPLES_DIR"/test_oop_stress.strada \
           "$EXAMPLES_DIR"/dumper_blessed.strada \
           "$EXAMPLES_DIR"/c_types_extended.strada \
           "$EXAMPLES_DIR"/test_enum.strada \
           "$EXAMPLES_DIR"/auto_optional.strada \
           "$EXAMPLES_DIR"/test_select.strada \
           "$EXAMPLES_DIR"/hash_complete.strada \
           "$EXAMPLES_DIR"/references_demo.strada \
           "$EXAMPLES_DIR"/refs_simple.strada \
           "$EXAMPLES_DIR"/refs_working.strada \
           "$EXAMPLES_DIR"/negative_index_values.strada \
           "$EXAMPLES_DIR"/array_hash.strada \
           "$EXAMPLES_DIR"/function_args_demo.strada; do
    name=$(basename "$src" .strada)
    if [ -f "$src" ]; then
        run_test "$src" "$name" "$name" "run" "" 5
    fi
done

# ---- Summary ----

echo ""
echo "========================================"
echo -e "Tests: $TOTAL  ${GREEN}Passed: $PASS${NC}  ${RED}Failed: $FAIL${NC}  ${YELLOW}Skipped: $SKIP${NC}"
echo "========================================"

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    echo -e "$ERRORS"
    echo ""
    exit 1
fi
exit 0
