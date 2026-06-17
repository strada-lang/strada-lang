#!/bin/bash
# t_tcc_shared.sh — `--tcc --shared` runtime-initialization regression
#
# Delegates to t/tcc_shared_test/run.sh: builds a shared library with
# --tcc, dlopens it from a host, and verifies our-globals and OOP method
# dispatch work — i.e. the wrapper's constructor shim compensates for tcc
# dropping __attribute__((constructor)). Skips (passes) when tcc isn't
# installed.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
ts_script="$SCRIPT_DIR/tcc_shared_test/run.sh"
if [ ! -x "$ts_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "tcc-shared-init" "runner not executable: $ts_script"
else
    ts_log="$BUILD_DIR/tcc_shared.log"
    if "$ts_script" > "$ts_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "tcc-shared-init (--tcc --shared constructor shim: globals + OOP at dlopen)"
    else
        FAILED=$((FAILED + 1))
        log_fail "tcc-shared-init" "see $ts_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$ts_log"
        fi
    fi
fi
