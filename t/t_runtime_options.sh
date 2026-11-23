#!/bin/bash
# t_runtime_options.sh — runtime settings are build-time options
#
# Delegates to t/runtime_options_test/run.sh: --no-gc, --exit-task-wait-ms,
# --no-pool, --debug-bless and --check-cleanup=warn on the compiler, the
# same switches on strada-interp, and the rule that no environment variable
# changes a built program.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
ro_script="$SCRIPT_DIR/runtime_options_test/run.sh"
if [ ! -x "$ro_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "runtime-options" "runner not executable: $ro_script"
else
    ro_log="$BUILD_DIR/runtime_options.log"
    if "$ro_script" > "$ro_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "runtime-options (--no-gc / --exit-task-wait-ms / --no-pool / --debug-bless; environment ignored)"
    else
        FAILED=$((FAILED + 1))
        log_fail "runtime-options" "see $ro_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$ro_log"
        fi
    fi
fi
