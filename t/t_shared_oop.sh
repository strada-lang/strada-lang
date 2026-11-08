#!/bin/bash
# t_shared_oop.sh — --shared libraries with namespaced has/extends classes
#
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
ts_script="$SCRIPT_DIR/shared_oop_test/run.sh"
if [ ! -x "$ts_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "shared-oop" "runner not executable: $ts_script"
else
    ts_log="$BUILD_DIR/shared_oop.log"
    if "$ts_script" > "$ts_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "shared-oop (--shared library inheritance + catchable missing methods)"
    else
        FAILED=$((FAILED + 1))
        log_fail "shared-oop" "see $ts_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$ts_log"
        fi
    fi
fi
