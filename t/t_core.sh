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

# Test: index()/substr() agree on byte-vs-char offset units (UTF-8 round-trip)
test_output_contains "$EXAMPLES_DIR/test_utf8_index_substr.strada" "test_utf8_index_substr" "All UTF-8 index/substr tests passed" "UTF-8 index/substr round-trip"

# Test: Control flow
test_run "$EXAMPLES_DIR/control_flow_demo.strada" "control_flow_demo" "Control flow"

# Test: Control flow 2 (unless, until, redo, statement modifiers)
test_output_contains "$EXAMPLES_DIR/test_control_flow2.strada" "test_control_flow2" "All control flow tests passed" "Control flow 2"

# Test: Array operations
test_run "$EXAMPLES_DIR/array_ops.strada" "array_ops" "Array operations"

# Test: References
test_run "$EXAMPLES_DIR/test_references.strada" "test_references" "References"
test_run "$EXAMPLES_DIR/test_refs.strada" "test_refs" "Refs"
test_output_contains "$EXAMPLES_DIR/test_weak_ref.strada" "test_weak_ref" "All weak reference tests passed" "Weak references"

# Test: Hash operations
test_run "$EXAMPLES_DIR/test_native_hash.strada" "test_native_hash" "Native hash"
test_run "$EXAMPLES_DIR/test_hash_barekeys.strada" "test_hash_barekeys" "Hash barekeys"
test_output_contains "$EXAMPLES_DIR/test_hash_refcount.strada" "test_hash_refcount" "All hash refcount tests passed" "Hash refcount"
test_output_contains "$EXAMPLES_DIR/anon_array_refcount.strada" "anon_array_refcount" "PASS: Anonymous array refcount test" "Anon array refcount"
test_output_contains "$EXAMPLES_DIR/temp_cleanup_test.strada" "temp_cleanup_test" "PASS: Temporary cleanup test completed" "Temp cleanup"

# Test: whole-array / whole-hash assignment to `our` (package-global) containers
# (single-element list wrap, our-hash lvalue, @our=@other aliasing — see fixture)
test_output_contains "$EXAMPLES_DIR/test_our_assign.strada" "test_our_assign" "All our-assign tests passed" "our array/hash assignment"

# Test: Parameter reassignment (tests that caller's values are not corrupted)
test_output_contains "$EXAMPLES_DIR/test_param_reassign.strada" "test_param_reassign" "All parameter reassignment tests passed" "Param reassign"

# Test: OOP
test_run "$EXAMPLES_DIR/test_oop.strada" "test_oop" "OOP basics"
test_output_contains "$EXAMPLES_DIR/test_dyn_dispatch.strada" "test_dyn_dispatch" "All dynamic dispatch tests passed" "Dynamic method dispatch"

# Test: Packages
test_run "$EXAMPLES_DIR/test_packages.strada" "test_packages" "Packages"

# Test: Regex
test_run "$EXAMPLES_DIR/test_regex.strada" "test_regex" "Regex"
test_output_contains "$EXAMPLES_DIR/test_pcre2.strada" "test_pcre2" "All PCRE2 tests passed" "PCRE2 regex"
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

# Test: Try/catch
test_run "$EXAMPLES_DIR/test_try_catch.strada" "test_try_catch" "Try/catch"
test_output_contains "$SCRIPT_DIR/test_exception_rethrow.strada" "test_exception_rethrow" "PASS: All 5 exceptions caught" "Exception re-throw"
# Test: lazy exception-trace capture (throw snapshots frames; core::exception_trace formats on read)
test_output_contains "$EXAMPLES_DIR/test_exception_trace_lazy.strada" "test_exception_trace_lazy" "exception_trace_lazy: all passed" "Lazy exception trace"

# Test: Stack traces on uncaught exceptions
test_exception_output "$SCRIPT_DIR/test_stack_trace.strada" "test_stack_trace" "Stack trace:" "Stack traces"

# Test: Const declarations
test_output_contains "$SCRIPT_DIR/test_const.strada" "test_const" "All const tests passed" "Const declarations"

# Test: Recursion limit
test_output_contains "$SCRIPT_DIR/test_recursion_limit.strada" "test_recursion_limit" "All recursion limit tests passed" "Recursion limit"

# Test: Perla gap fixes (compound assign, our++, shift(), int(), hash slurp, hash from array, extends+new)
test_output_contains "$EXAMPLES_DIR/test_perla_gaps.strada" "test_perla_gaps" "All Perla gap tests passed" "Perla gap fixes"

# Test: Regex literal as function argument (split(/,/, $str))
test_output_contains "$EXAMPLES_DIR/test_regex_split.strada" "test_regex_split" "All regex split tests passed" "Regex literal in func args"

# Test: Goto and loop labels
test_run "$EXAMPLES_DIR/test_goto.strada" "test_goto" "Goto"
test_run "$EXAMPLES_DIR/test_loop_labels.strada" "test_loop_labels" "Loop labels"
test_run "$EXAMPLES_DIR/test_do_while.strada" "test_do_while" "Do-while loops"

# Test: Foreach
test_run "$EXAMPLES_DIR/test_foreach.strada" "test_foreach" "Foreach loops"

# Test: JSON
test_run "$EXAMPLES_DIR/test_json.strada" "test_json" "JSON"

# Test: JSON C implementation vs JSON::PS differential equivalence
test_output_contains "$EXAMPLES_DIR/test_json_differential.strada" "test_json_differential" "PASS: JSON / JSON::PS differential" "JSON C/PS differential"

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

# Test: dumper()/clone() depth cap + cycle detection (CWE-674 stack-overflow DoS)
test_output_contains "$EXAMPLES_DIR/test_dumper_clone_depth.strada" "test_dumper_clone_depth" "all passed" "Dumper/clone recursion guard"

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
test_output_contains "$EXAMPLES_DIR/test_in_memory_io.strada" "test_in_memory_io" "All in-memory I/O tests passed" "In-memory I/O (open \$scalar)"
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

# Test: Int arithmetic optimization and inline constructors
test_output "$EXAMPLES_DIR/test_int_arith_opt.strada" "test_int_arith_opt" "add: 30
mul: 200
sub: 10
nested: 500
sum: 4950
point dist_sq: 25
point3d dist_sq: 50
default x: 0
default y: 0
explicit z: 10
7^2+24^2: 625
big: 1000000000000
isa Point: 1
isa Point3D: 1
count: 10
all tests passed" "Int arithmetic and inline constructors"

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

# Test: DateTime library
test_output_contains "$EXAMPLES_DIR/test_datetime.strada" "test_datetime" "All DateTime tests passed" "DateTime library"

# Test: Nesso ORM (requires SQLite)
SAVED_EXTRA_LDFLAGS="$EXTRA_LDFLAGS"
EXTRA_LDFLAGS="$EXTRA_LDFLAGS -lsqlite3"
test_output_contains "lib/Nesso/test_nesso.strada" "test_nesso" "PASS: All Nesso tests passed" "Nesso ORM"
# Test: Nesso identifier injection (ORDER BY / condition-key SQLi, CWE-89)
test_output_contains "lib/Nesso/test_nesso_sqli.strada" "test_nesso_sqli" "PASS: All Nesso SQLi tests passed" "Nesso SQLi guard"
# Test: DBI::quote (native escaper) + DBI::quote_identifier
test_output_contains "lib/dbi/test_dbi_quote.strada" "test_dbi_quote" "PASS: All DBI quote tests passed" "DBI quote/quote_identifier"
EXTRA_LDFLAGS="$SAVED_EXTRA_LDFLAGS"

# Test: Nested use statements (modules that use other modules)
test_output_contains "$SCRIPT_DIR/test_nested_use.strada" "test_nested_use" "All nested use tests passed" "Nested use"

# Test: OOP with nested use statements
test_output_contains "$SCRIPT_DIR/test_nested_oop.strada" "test_nested_oop" "All nested OOP tests passed" "Nested OOP"

# Test: OOP with import_lib
test_import_lib "$SCRIPT_DIR/test_import_lib_oop.strada" "test_import_lib_oop" "$SCRIPT_DIR/nested_use_test/OOPLib.strada" "OOPLib" "import_lib OOP"

# Test: lazily-loaded .so registering a modifier on a HOST class invalidates
# warmed dispatch caches (generation-counter invalidation across the boundary)
test_import_lib "$SCRIPT_DIR/test_import_lib_hooks.strada" "test_import_lib_hooks" "$SCRIPT_DIR/nested_use_test/HookLib.strada" "HookLib" "import_lib hook invalidation"

# Test: OOP with import_object
test_import_object "$SCRIPT_DIR/test_import_object_oop.strada" "test_import_object_oop" "$SCRIPT_DIR/nested_use_test/OOPLib.strada" "OOPLib" "import_object OOP"

# Test: OOP with import_archive
test_import_archive "$SCRIPT_DIR/test_import_archive_oop.strada" "test_import_archive_oop" "$SCRIPT_DIR/nested_use_test/OOPLib.strada" "OOPLib" "import_archive OOP"

# Test: Moose-style OOP (has, extends, before/after modifiers)
test_output "$EXAMPLES_DIR/test_moose.strada" "test_moose" "Rex
3
4
100
80
dog (energy: 80)
[preparing to bark]
Rex barks!
[done barking]
1
1" "Moose-style OOP"

# Test: Dynamic return type with wantarray/wantscalar/wanthash
test_output_contains "$EXAMPLES_DIR/test_dynamic.strada" "test_dynamic" "ok" "Dynamic return type"

# Test: UTF-8 namespace functions
test_output_contains "$EXAMPLES_DIR/test_utf8_ns.strada" "test_utf8_ns" "10 passed, 0 failed" "UTF-8 namespace functions"
test_output_contains "$EXAMPLES_DIR/test_vec.strada" "test_vec" "All vec tests passed" "vec() bit-vector access"

# Test: Array and hash slices
test_output_contains "$EXAMPLES_DIR/test_slices.strada" "test_slices" "All slice tests passed" "Array/hash slices"

# Test: hash variable-key compound assign ($h{$k} += / .=) — regression for a
# crash (wrong store fn took StradaHash* where a StradaValue* was passed) and
# a UAF (borrowed key was decref'd). Must not crash and must read back right.
test_output "$EXAMPLES_DIR/test_hash_compound_varkey.strada" "test_hash_compound_varkey" "ok x y" "Hash variable-key compound assign"

# Test: sort { ... } uses an O(n log n) stable merge sort (was O(n^2) bubble).
test_output "$EXAMPLES_DIR/test_sort_mergesort.strada" "test_sort_mergesort" "all sort tests passed" "Comparator sort is O(n log n) + stable"

# Test: Our variable declarations
test_output_contains "$EXAMPLES_DIR/test_our.strada" "test_our" "All our variable tests passed" "Our variables"

# Test: BEGIN/END blocks
test_output_contains "$EXAMPLES_DIR/test_begin_end.strada" "test_begin_end" "PASS: BEGIN/END blocks" "BEGIN/END blocks"

# Test: AUTOLOAD
test_output_contains "$EXAMPLES_DIR/test_autoload.strada" "test_autoload" "All AUTOLOAD tests passed" "AUTOLOAD"

# Test: Operator overloading
test_output_contains "$EXAMPLES_DIR/test_overload.strada" "test_overload" "All overload tests passed" "Overload"

# Test: Math::BigInt
test_output_contains "$EXAMPLES_DIR/test_bigint.strada" "test_bigint" "All BigInt tests passed" "BigInt"

# Test: Math::BigFloat
test_output_contains "$EXAMPLES_DIR/test_bigfloat.strada" "test_bigfloat" "All BigFloat tests passed" "BigFloat"

# Test: String repeat (x operator)
test_output_contains "$EXAMPLES_DIR/test_str_repeat.strada" "test_str_repeat" "All str repeat tests passed" "String repeat"

# Test: splice()
test_output_contains "$EXAMPLES_DIR/test_splice.strada" "test_splice" "All splice tests passed" "Splice"

# Test: each(%h)
test_output_contains "$EXAMPLES_DIR/test_each.strada" "test_each" "All each tests passed" "Each"

# Test: select()
test_output_contains "$EXAMPLES_DIR/test_select_fh.strada" "test_select_fh" "All select tests passed" "Select"

# Test: tr///
test_output_contains "$EXAMPLES_DIR/test_tr.strada" "test_tr" "All tr tests passed" "Transliteration"

# Test: local()
test_output_contains "$EXAMPLES_DIR/test_local.strada" "test_local" "All local tests passed" "Local"

# Test: /e regex modifier
test_output_contains "$EXAMPLES_DIR/test_regex_eval.strada" "test_regex_eval" "All regex eval tests passed" "Regex eval"

# Test: tie/untie/tied
test_output_contains "$EXAMPLES_DIR/test_tie.strada" "test_tie" "All tie tests passed" "Tie"

# Test: Perl-style sigil semantics (scalar context, list flattening, optional types)
test_output_contains "$EXAMPLES_DIR/test_perl_sigils.strada" "test_perl_sigils" "All Perl sigil tests passed" "Perl sigils"

# Test: No-parens function definition with implicit @_
test_output_contains "$EXAMPLES_DIR/test_noparens_func.strada" "test_noparens_func" "All no-parens func tests passed" "No-parens func"

# Test: Implicit $_ in foreach
test_output_contains "$EXAMPLES_DIR/test_implicit_underscore.strada" "test_implicit_underscore" "All implicit" "Implicit \$_"

# Test: Autovivification ($h{"a"}{"b"} = val auto-creates intermediate hashes)
test_output_contains "$EXAMPLES_DIR/test_autovivification.strada" "test_autovivification" "All autovivification tests passed" "Autovivification"

# Test: *= and /= compound assignment (regression: previously didn't lex)
test_output_contains "$EXAMPLES_DIR/test_muldiv_assign.strada" "test_muldiv_assign" "All muldiv assign tests passed" "Mul/div compound assign"

# Test: %=, **=, //=, x= compound assignment (regression: didn't lex/parse)
test_output_contains "$EXAMPLES_DIR/test_compound_ops.strada" "test_compound_ops" "All compound op tests passed" "Compound op family"

# Test: malformed base64 input must not corrupt memory (SECURITY_AUDIT
# finding #1 — integer underflow → OOB heap write; guarded in runtime)
test_output_contains "$EXAMPLES_DIR/test_base64_malformed.strada" "test_base64_malformed" "All malformed-base64 safety tests passed" "Base64 malformed-input safety"

# Test: core::byte(n) raw-byte builtin + binary base64 round-trip (chr() is
# codepoint-oriented; byte() is the byte-oriented counterpart)
test_output_contains "$EXAMPLES_DIR/test_byte_builtin.strada" "test_byte_builtin" "All byte() builtin tests passed" "byte() raw-byte builtin"

# Test: int-declared storage holds canonical ints (regression: string/NUM
# values were stored as-is, confusing the int fast paths)
test_output_contains "$EXAMPLES_DIR/test_int_coercion.strada" "test_int_coercion" "All int coercion tests passed" "Int variable coercion"

# Test: around hooks receive ($orig, $self, @args) per Moose convention
# (regression: $orig/$self were swapped, so $self->accessor() read empty)
test_output_contains "$EXAMPLES_DIR/test_around_hooks.strada" "test_around_hooks" "All around hook tests passed" "Around method modifiers"

# Test: Perl compatibility (chomp, bare shift, heredocs, unless-elsif)
test_output_contains "$EXAMPLES_DIR/test_perl_compat.strada" "test_perl_compat" "All Perl compat tests passed" "Perl compat"

# Test: String eval (use Eval)
test_output_contains "$EXAMPLES_DIR/test_eval.strada" "test_eval" "All eval tests passed" "String eval"
test_output_contains "$EXAMPLES_DIR/test_unicode_normalize.strada" "test_unicode_normalize" "normalize: 5 passed, 0 failed" "Unicode normalization (NFC/NFD/NFKC/NFKD)"

# Test: round-5 perf work (zero-copy keys COW, join_sv, decorated sort,
# match-data reuse, condition CSE, growable sprintf)
test_output_contains "$EXAMPLES_DIR/test_perf_round5.strada" "test_perf_round5" "All round5 perf tests passed" "Round-5 perf regressions"

# Test: flattened multi-part concat (strada_concat_multi — interpolation chains)
test_output_contains "$EXAMPLES_DIR/test_concat_multi.strada" "test_concat_multi" "All concat-multi tests passed" "Multi-part concat flattening"

# Test: self-append in-place optimization ($s = $s . a . b . ... -> concat_inplace)
test_output_contains "$EXAMPLES_DIR/test_self_append.strada" "test_self_append" "test_self_append: all passed" "Self-append in-place concat"

# Test: anon-array presize + direct-fill (sizes, empty-then-push, presize-then-grow, nested)
test_output_contains "$EXAMPLES_DIR/test_anon_array_presize.strada" "test_anon_array_presize" "test_anon_array_presize: all passed" "Anon-array presize/direct-fill"
# Test: compact single-block arrays — inline->heap migration on grow (push/unshift/splice/index-set/pop-then-grow)
test_output_contains "$EXAMPLES_DIR/test_compact_array.strada" "test_compact_array" "compact_stress: all passed" "Compact single-block arrays"

# Test: Perl-style list flattening + our-array mutation (codegen bugs fixed 2026-06)
test_output_contains "$EXAMPLES_DIR/test_list_flatten.strada" "test_list_flatten" "All list-flatten tests passed" "List flattening + our-array push"

# Test: mutating `our` (package-global) composites from functions (init-form bugs fixed 2026-06)
test_output_contains "$EXAMPLES_DIR/test_our_globals.strada" "test_our_globals" "All our-global tests passed" "our-global array/hash/scalar mutation"

# Test: hardened request arena (nested containers + values escaping arena_end)
test_output_contains "$EXAMPLES_DIR/test_arena_hardened.strada" "test_arena_hardened" "All arena-hardened tests passed" "Arena nested/escaping correctness"

# Test: the in-language Test framework (TAP output, exit codes)
test_output_contains "$EXAMPLES_DIR/test_test_framework.strada" "test_test_framework" "1..10" "Test framework (TAP)"
test_exit_code "$EXAMPLES_DIR/test_test_framework_fails.strada" "test_test_framework_fails" 1 "Test framework failure exit"

# Test: lazy ranges in map/grep (native iteration, no materialization)
test_output_contains "$EXAMPLES_DIR/test_lazy_range.strada" "test_lazy_range" "1..8" "Lazy ranges in map/grep"

# Test: value-producing do {} blocks
test_output_contains "$EXAMPLES_DIR/test_do_expr.strada" "test_do_expr" "1..9" "Value-producing do blocks"

# Test: error chaining (Exception objects, core::exception_trace)
test_output_contains "$EXAMPLES_DIR/test_exception_chain.strada" "test_exception_chain" "1..12" "Error chaining"

# Test: List:: helpers (reduce/any/all/first/sum/min/max/uniq/zip/pairs)
test_output_contains "$EXAMPLES_DIR/test_list_util.strada" "test_list_util" "1..23" "List utilities"

# Test: try/catch/finally semantics (normal, caught, unmatched-rethrow,
# catch-throws, no-catch, return-crossing, loop next/last)
test_output_contains "$EXAMPLES_DIR/test_finally.strada" "test_finally" "All finally tests passed" "try/finally"

# Test: concurrency ergonomics (async::select/spawn/sleep/map,
# thread::tls_*, Async::Scope nursery, Async::Actor)
test_output_contains "$EXAMPLES_DIR/test_async_ergonomics.strada" "test_async_ergonomics" "All async ergonomics tests passed" "Concurrency ergonomics" 30

# Test: per-thread runtime state (atomic SS refcounts, per-call to_str
# scratch, thread-local regex captures/$1 and call stacks)
test_output_contains "$EXAMPLES_DIR/test_thread_state.strada" "test_thread_state" "All thread-state tests passed" "Thread-local runtime state"

# Test: pooled match-data capture clamping (stale ovector slots from an
# earlier bigger match must not leak; trailing optional groups keep undef)
test_output_contains "$EXAMPLES_DIR/test_regex_pool_captures.strada" "test_regex_pool_captures" "All pool-capture tests passed" "Pooled regex capture clamping"

# Test: c::callback libffi trampolines (qsort comparator, marshaling, free).
# Requires libffi at build time — skip cleanly when configured out.
if grep -q "^export STRADA_HAVE_LIBFFI=1" "$PROJECT_DIR/config.sh" 2>/dev/null; then
    test_output_contains "$EXAMPLES_DIR/test_c_callback.strada" "test_c_callback" "All c::callback tests passed" "c::callback (libffi trampolines)"
else
    test_skip "c::callback (libffi trampolines)" "built without libffi"
fi

# Test: Async::Loop epoll event loop + green tasks (Linux/epoll only —
# skip cleanly when configure detected no epoll support).
if grep -q "^export STRADA_HAVE_EPOLL=1" "$PROJECT_DIR/config.sh" 2>/dev/null; then
    test_output_contains "$EXAMPLES_DIR/test_event_loop.strada" "test_event_loop" "1..29" "Async::Loop event loop + green tasks" 30
else
    test_skip "Async::Loop event loop + green tasks" "built without epoll"
fi

# Test: transitive closure capture (capture-of-capture through nested closures)
test_output_contains "$EXAMPLES_DIR/test_nested_closures.strada" "test_nested_closures" "1..5" "Nested closure capture"

# Test: TLS over green tasks (self-skips without the openssl CLI; gated on
# OpenSSL being available at build time)
if grep -q '^export STRADA_SSL_LIBS=..*-lssl' "$PROJECT_DIR/config.sh" 2>/dev/null; then
    test_output_contains "$EXAMPLES_DIR/test_event_loop_ssl.strada" "test_event_loop_ssl" "1.." "TLS over green tasks" 30
    # ssl::attach_fd must refuse verify=1 with no hostname to bind (MITM guard)
    test_output_contains "$EXAMPLES_DIR/test_ssl_attach_verify.strada" "test_ssl_attach_verify" "PASS: All ssl attach_fd tests passed" "SSL attach_fd hostname-binding guard"
else
    test_skip "TLS over green tasks" "built without OpenSSL"
    test_skip "SSL attach_fd hostname-binding guard" "built without OpenSSL"
fi

# Test: namespaced builtin aliases (re::/str::/sb:: and core::-qualified
# spellings of historically-unqualified builtins; legacy names still work)
test_output_contains "$EXAMPLES_DIR/test_namespaced_builtins.strada" "test_namespaced_builtins" "1..25" "Namespaced builtin aliases"

# Test: --strict-types stage-0 type warnings (warning-only, gradual).
# Expects exactly 5 warnings from the fixture's flagged lines, zero
# without the flag, and successful compilation either way.
TOTAL=$((TOTAL + 1))
STRICT_OUT=$("$STRADAC" --strict-types "$EXAMPLES_DIR/test_strict_types.strada" "$BUILD_DIR/test_strict_types.c" 2>&1)
STRICT_RC=$?
STRICT_WARNS=$(echo "$STRICT_OUT" | grep -c "^warning:")
NOFLAG_WARNS=$("$STRADAC" "$EXAMPLES_DIR/test_strict_types.strada" "$BUILD_DIR/test_strict_types.c" 2>&1 | grep -c "^warning:")
if [ "$STRICT_RC" -eq 0 ] && [ "$STRICT_WARNS" -eq 5 ] && [ "$NOFLAG_WARNS" -eq 0 ]; then
    log_pass "compile: --strict-types warnings (5 expected, 0 without flag)"
    PASSED=$((PASSED + 1))
else
    log_fail "compile: --strict-types warnings" "rc=$STRICT_RC warns=$STRICT_WARNS noflag=$NOFLAG_WARNS"
    FAILED=$((FAILED + 1))
fi
