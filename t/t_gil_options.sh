#!/bin/bash
# t_gil_options.sh — thread lock settings are build-time options
#
# Delegates to t/gil_options_test/run.sh: --no-gil, --gil-interval-us and
# --gil-polls on the compiler, the same options on strada-interp, and the
# rule that no environment variable changes a built program.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
go_script="$SCRIPT_DIR/gil_options_test/run.sh"
if [ ! -x "$go_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "gil-options" "runner not executable: $go_script"
else
    go_log="$BUILD_DIR/gil_options.log"
    if "$go_script" > "$go_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "gil-options (--no-gil / --gil-interval-us / --gil-polls; environment ignored)"
    else
        FAILED=$((FAILED + 1))
        log_fail "gil-options" "see $go_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$go_log"
        fi
    fi
fi
