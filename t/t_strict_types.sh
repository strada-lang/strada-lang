#!/bin/bash
# t_strict_types.sh — checked types
#
# Delegates to t/strict_types_test/run.sh: `use strict "types";` turns type
# mismatches into compile errors for one file, array<T> / hash<T> element
# types are checked, and strada-interp rejects the same programs.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
st_script="$SCRIPT_DIR/strict_types_test/run.sh"
if [ ! -x "$st_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "strict-types" "runner not executable: $st_script"
else
    st_log="$BUILD_DIR/strict_types.log"
    if "$st_script" > "$st_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "strict-types (use strict \"types\", array<T> / hash<T>, per-file scope, VM agrees)"
    else
        FAILED=$((FAILED + 1))
        log_fail "strict-types" "see $st_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$st_log"
        fi
    fi
fi
