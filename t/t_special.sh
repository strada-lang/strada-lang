#!/bin/bash
# t_special.sh - Special and edge case tests
#
# Tests for edge cases, error handling, and special scenarios

# Test: Deref syntax variations
test_run "$EXAMPLES_DIR/test_deref_array.strada" "test_deref_array" "Deref array"
test_run "$EXAMPLES_DIR/test_deref_syntax.strada" "test_deref_syntax" "Deref syntax"
test_run "$EXAMPLES_DIR/test_deref_set.strada" "test_deref_set" "Deref set"
test_run "$EXAMPLES_DIR/deref_expr.strada" "deref_expr" "Deref expressions"

# Test: Perl-style references
test_run "$EXAMPLES_DIR/test_perl_refs.strada" "test_perl_refs" "Perl refs"

# Test: Pass by reference
test_run "$EXAMPLES_DIR/pass_by_reference.strada" "pass_by_reference" "Pass by reference"

# Test: qq strings
test_run "$EXAMPLES_DIR/test_qq_strings.strada" "test_qq_strings" "QQ strings"

# Test: External packages
test_run "$EXAMPLES_DIR/test_extern_pkg.strada" "test_extern_pkg" "Extern packages"
test_run "$EXAMPLES_DIR/test_modules.strada" "test_modules" "Modules"

# Test: FFI
test_run "$EXAMPLES_DIR/test_ffi.strada" "test_ffi" "FFI"

# Test: Math library
test_run "$EXAMPLES_DIR/mathlib.strada" "mathlib" "Math library"

# Test: OOP better
test_run "$EXAMPLES_DIR/test_oop_better.strada" "test_oop_better" "OOP improved"

# Test: OOP stress
test_run "$EXAMPLES_DIR/oop_stress_test.strada" "oop_stress_test" "OOP stress"

# Test: Dumper with blessed objects
test_run "$EXAMPLES_DIR/dumper_blessed.strada" "dumper_blessed" "Dumper blessed"

# Test: Extended C types (int8, int16, uint8, uint16, uint32, uint64, size_t, char)
test_run "$EXAMPLES_DIR/c_types_extended.strada" "c_types_extended" "C types extended"

# Test: Enum types
test_run "$EXAMPLES_DIR/test_enum.strada" "test_enum" "Enum types"

# Test: Auto optional
test_run "$EXAMPLES_DIR/auto_optional.strada" "auto_optional" "Auto optional"

# Test: Select (non-server)
test_run "$EXAMPLES_DIR/test_select.strada" "test_select" "Select" 3

# Test: Complete hash
test_run "$EXAMPLES_DIR/test_hash_complete.strada" "test_hash_complete" "Hash complete"

# Test: References variations
test_run "$EXAMPLES_DIR/test_references_demo.strada" "test_references_demo" "References demo"
test_run "$EXAMPLES_DIR/test_refs_simple.strada" "test_refs_simple" "Refs simple"
test_run "$EXAMPLES_DIR/test_refs_working.strada" "test_refs_working" "Refs working"

# Test: Negative index values
test_run "$EXAMPLES_DIR/negative_index_values.strada" "negative_index_values" "Negative index values"

# Test: Array hash
test_run "$EXAMPLES_DIR/test_array_hash.strada" "test_array_hash" "Array hash"

# Test: Function args demo
test_run "$EXAMPLES_DIR/function_args_demo.strada" "function_args_demo" "Function args demo"

# Test: Example program
test_run "$EXAMPLES_DIR/example.strada" "example" "Example program"
