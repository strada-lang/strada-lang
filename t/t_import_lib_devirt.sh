#!/bin/bash
# t_import_lib_devirt.sh — guarded cross-.so devirtualization regression
#
# Delegates to t/import_lib_devirt_test/run.sh: builds a .so class +
# host, verifies method calls devirtualize to direct wrapper calls
# (and that hook-bearing methods don't), then swaps in a changed .so and
# verifies the fingerprint mismatch routes every call through dynamic
# dispatch — new behavior and new hooks visible, no crash.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
dv_script="$SCRIPT_DIR/import_lib_devirt_test/run.sh"
if [ ! -x "$dv_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "import-lib-devirt" "runner not executable: $dv_script"
else
    dv_log="$BUILD_DIR/import_lib_devirt.log"
    if "$dv_script" > "$dv_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "import-lib-devirt (cross-.so direct calls + hook gating + swap fallback)"
    else
        FAILED=$((FAILED + 1))
        log_fail "import-lib-devirt" "see $dv_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$dv_log"
        fi
    fi
fi
