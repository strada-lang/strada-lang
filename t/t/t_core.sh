#!/bin/bash
# t_core.sh - Core functionality tests
#
# Tests for basic Strada features with output verification

# Test: Basic arithmetic and functions
test_output "$EXAMPLES_DIR/test_simple.strada" "test_simple" "30" "Basic arithmetic"

# Test: Exit codes
test_exit_code "$EXAMPLES_DIR/test_exit.strada" "test_exit" 0 "Exit success"
test_exit_code "$EXAMPLES_DIR/test_exit_error.strada" "test_exit_error" 1 "Exit with error"

# Test: String functions - check for key output patterns
test_output_contains "$EXAMPLES_DIR/test_strings.strada" "test_strings" "All tests passed" "String functions"

# Test: Struct demo
test_run "$EXAMPLES_DIR/struct_demo.strada" "struct_demo" "Struct demo"

# Test: Control flow
test_run "$EXAMPLES_DIR/control_flow_demo.strada" "control_flow_demo" "Control flow"

# Test: Array operations
test_run "$EXAMPLES_DIR/array_ops.strada" "array_ops" "Array operations"

# Test: References
test_run "$EXAMPLES_DIR/test_references.strada" "test_references" "References"
test_run "$EXAMPLES_DIR/test_refs.strada" "test_refs" "Refs"

# Test: Hash operations
test_run "$EXAMPLES_DIR/test_native_hash.strada" "test_native_hash" "Native hash"
test_run "$EXAMPLES_DIR/test_hash_barekeys.strada" "test_hash_barekeys" "Hash barekeys"
test_output_contains "$EXAMPLES_DIR/test_hash_refcount.strada" "test_hash_refcount" "All hash refcount tests passed" "Hash refcount"
test_output_contains "$EXAMPLES_DIR/anon_array_refcount.strada" "anon_array_refcount" "PASS: Anonymous array refcount test" "Anon array refcount"
test_output_contains "$EXAMPLES_DIR/temp_cleanup_test.strada" "temp_cleanup_test" "PASS: Temporary cleanup test completed" "Temp cleanup"

# Test: Parameter reassignment (tests that caller's values are not corrupted)
test_output_contains "$EXAMPLES_DIR/test_param_reassign.strada" "test_param_reassign" "All parameter reassignment tests passed" "Param reassign"

# Test: OOP
test_run "$EXAMPLES_DIR/test_oop.strada" "test_oop" "OOP basics"

# Test: Packages
test_run "$EXAMPLES_DIR/test_packages.strada" "test_packages" "Packages"

# Test: Regex
test_run "$EXAMPLES_DIR/test_regex.strada" "test_regex" "Regex"
test_run "$EXAMPLES_DIR/test_inline_regex.strada" "test_inline_regex" "Inline regex"

# Test: Optional parameters
test_run "$EXAMPLES_DIR/optional_params.strada" "optional_params" "Optional params"
test_run "$EXAMPLES_DIR/optional_simple.strada" "optional_simple" "Optional simple"

# Test: Variable arguments
test_run "$EXAMPLES_DIR/variable_args_demo.strada" "variable_args_demo" "Variable args"
test_run "$EXAMPLES_DIR/spread_operator.strada" "spread_operator" "Spread operator"

# Test: Variadic functions
test_run "$EXAMPLES_DIR/test_variadic.strada" "test_variadic" "Variadic functions"
test_run "$EXAMPLES_DIR/test_variadic_oop.strada" "test_variadic_oop" "Variadic OOP methods"

# Test: import_lib with variadic functions
test_import_lib "$EXAMPLES_DIR/test_import_variadic.strada" "test_import_variadic" "$EXAMPLES_DIR/VariadicLib.strada" "VariadicLib" "import_lib variadic"

# Test: import_object with variadic functions
test_import_object "$EXAMPLES_DIR/test_import_object_variadic.strada" "test_import_object_variadic" "$EXAMPLES_DIR/VariadicObjLib.strada" "VariadicObjLib" "import_object variadic"

# Test: Negative indexing
test_run "$EXAMPLES_DIR/negative_index.strada" "negative_index" "Negative index"

# Test: Function pointers and callbacks
test_run "$EXAMPLES_DIR/test_funcptr.strada" "test_funcptr" "Function pointers"
test_run "$EXAMPLES_DIR/test_callbacks.strada" "test_callbacks" "Callbacks"
test_output_contains "$EXAMPLES_DIR/test_void_funcptr.strada" "test_void_funcptr" "PASS: All void function pointer tests completed" "Void funcptr"

# Test: Type introspection
test_run "$EXAMPLES_DIR/test_clone.strada" "test_clone" "Clone"

# Test: Try/catch
test_run "$EXAMPLES_DIR/test_try_catch.strada" "test_try_catch" "Try/catch"
test_output_contains "$SCRIPT_DIR/test_exception_rethrow.strada" "test_exception_rethrow" "PASS: All 5 exceptions caught" "Exception re-throw"

# Test: Goto and loop labels
test_run "$EXAMPLES_DIR/test_goto.strada" "test_goto" "Goto"
test_run "$EXAMPLES_DIR/test_loop_labels.strada" "test_loop_labels" "Loop labels"
test_run "$EXAMPLES_DIR/test_do_while.strada" "test_do_while" "Do-while loops"

# Test: Foreach
test_run "$EXAMPLES_DIR/test_foreach.strada" "test_foreach" "Foreach loops"

# Test: JSON
test_run "$EXAMPLES_DIR/test_json.strada" "test_json" "JSON"

# Test: Sort
test_run "$EXAMPLES_DIR/test_sort.strada" "test_sort" "Sort"
test_run "$EXAMPLES_DIR/test_map_sort.strada" "test_map_sort" "Map sort"

# Test: Magic constants
test_run "$EXAMPLES_DIR/test_magic_constants.strada" "test_magic_constants" "Magic constants"

# Test: Universal methods
test_run "$EXAMPLES_DIR/test_universal_isa.strada" "test_universal_isa" "ISA method"
test_run "$EXAMPLES_DIR/test_universal_can.strada" "test_universal_can" "CAN method"

# Test: Dumper
test_run "$EXAMPLES_DIR/dumper_demo.strada" "dumper_demo" "Dumper demo"

# Test: Free/memory
test_run "$EXAMPLES_DIR/test_free.strada" "test_free" "Memory free"

# Test: File operations
test_run "$EXAMPLES_DIR/test_file_write.strada" "test_file_write" "File write"

# Test: Process operations
test_run "$EXAMPLES_DIR/test_process.strada" "test_process" "Process ops"
test_run "$EXAMPLES_DIR/test_system.strada" "test_system" "System calls"

# Test: POSIX functions
test_run "$EXAMPLES_DIR/test_posix.strada" "test_posix" "POSIX functions"

# Test: Signals (only compile - requires manual signal sending)
test_compile "$EXAMPLES_DIR/test_signals.strada" "test_signals" "Signal handling"

# Test: C integration
test_run "$EXAMPLES_DIR/c_shared_lib.strada" "c_shared_lib" "C shared library"

# Test: Struct types
test_run "$EXAMPLES_DIR/test_struct.strada" "test_struct" "Struct types"
test_run "$EXAMPLES_DIR/test_struct_types.strada" "test_struct_types" "Struct type system"
test_run "$EXAMPLES_DIR/native_struct_full.strada" "native_struct_full" "Native structs full"

# Test: New libc functions (2026-01-03)
test_run "$EXAMPLES_DIR/test_math.strada" "test_math" "Math functions"
test_run "$EXAMPLES_DIR/test_directory.strada" "test_directory" "Directory functions"
test_run "$EXAMPLES_DIR/test_path.strada" "test_path" "Path functions"
test_run "$EXAMPLES_DIR/test_seek.strada" "test_seek" "File seek functions"
test_run "$EXAMPLES_DIR/test_dns.strada" "test_dns" "DNS functions"

# Test: Extended libc functions (2026-01-04)
test_output_contains "$EXAMPLES_DIR/test_math_libc.strada" "test_math_libc" "All math libc tests passed" "Extended math functions"
test_output_contains "$EXAMPLES_DIR/test_process_libc.strada" "test_process_libc" "All process libc tests passed" "Extended process functions"
test_output_contains "$EXAMPLES_DIR/test_user_libc.strada" "test_user_libc" "All user libc tests passed" "User/group functions"
test_output_contains "$EXAMPLES_DIR/test_fileio_libc.strada" "test_fileio_libc" "All file I/O libc tests passed" "File I/O functions"
test_output_contains "$EXAMPLES_DIR/test_socket_libc.strada" "test_socket_libc" "All socket libc tests passed" "Socket functions"
test_output_contains "$EXAMPLES_DIR/test_misc_libc.strada" "test_misc_libc" "All misc libc tests passed" "Misc libc functions"

# Test: Closures and anonymous functions
# Note: test_closures.strada tests capture-by-reference mutation which was
# changed to capture-by-value for thread safety - compile only
test_compile "$EXAMPLES_DIR/test_closures.strada" "test_closures" "Closures"
test_output_contains "$EXAMPLES_DIR/test_closure_params.strada" "test_closure_params" "All closure parameter tests passed" "Closure params"

# Test: Operators
test_run "$EXAMPLES_DIR/test_operators.strada" "test_operators" "Operators"

# Test: Switch statement
test_run "$EXAMPLES_DIR/test_switch.strada" "test_switch" "Switch statement"

# Test: Ternary operator
test_run "$EXAMPLES_DIR/test_ternary.strada" "test_ternary" "Ternary operator"

# Test: Range operator
test_run "$EXAMPLES_DIR/test_range.strada" "test_range" "Range operator"

# Test: Namespaces
test_run "$EXAMPLES_DIR/test_namespaces.strada" "test_namespaces" "Namespaces"

# Test: Multiple inheritance
test_run "$EXAMPLES_DIR/test_multi_inherit.strada" "test_multi_inherit" "Multiple inheritance"

# Test: More OOP
test_run "$EXAMPLES_DIR/test_oop2.strada" "test_oop2" "OOP extended"

# Test: Full package features
test_run "$EXAMPLES_DIR/test_package_full.strada" "test_package_full" "Package full"

# Test: Boolean context
test_run "$EXAMPLES_DIR/test_bool_context.strada" "test_bool_context" "Boolean context"

# Test: String interpolation
test_run "$EXAMPLES_DIR/test_interpolation.strada" "test_interpolation" "String interpolation"

# Test: File slurp
test_run "$EXAMPLES_DIR/test_slurp.strada" "test_slurp" "File slurp"

# Test: ARGV handling
test_run "$EXAMPLES_DIR/test_argv.strada" "test_argv" "ARGV handling"

# Test: CSV parsing
test_run "$EXAMPLES_DIR/text_csv_demo.strada" "text_csv_demo" "CSV parsing"

# Test: Memory management
test_run "$EXAMPLES_DIR/test_memory.strada" "test_memory" "Memory management"

# Test: Command line tools (compile only - they need args)
test_compile "$EXAMPLES_DIR/ls.strada" "ls" "ls command"
test_compile "$EXAMPLES_DIR/ps.strada" "ps" "ps command"

# Test: LWP library
test_output_contains "$EXAMPLES_DIR/test_lwp.strada" "test_lwp" "All LWP tests passed" "LWP HTTP library"

# Test: Nesso ORM (requires SQLite)
EXTRA_LDFLAGS="-lsqlite3"
test_output_contains "lib/Nesso/test_nesso.strada" "test_nesso" "PASS: All Nesso tests passed" "Nesso ORM"
EXTRA_LDFLAGS=""
