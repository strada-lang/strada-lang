#!/bin/bash
# t_codegen_escape.sh — codegen C-injection + driver CC-injection regression
#
# Delegates to t/codegen_escape_test/run.sh: verifies the three source-derived
# string-emission sites (tie classname, __FILE__, #line filename) route through
# the gen_str_literal_c escaper instead of raw-concatenating into C (CWE-94),
# and that the build driver rejects a CC env var carrying shell metacharacters
# (CWE-78). Counts the runner as a single pass/fail test.

TOTAL=$((TOTAL + 1))
ce_script="$SCRIPT_DIR/codegen_escape_test/run.sh"
if [ ! -x "$ce_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "codegen-escape" "runner not executable: $ce_script"
else
    ce_log="$BUILD_DIR/codegen_escape.log"
    if "$ce_script" > "$ce_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "codegen-escape (tie/__FILE__/#line C-injection + driver CC shell-injection guards)"
    else
        FAILED=$((FAILED + 1))
        log_fail "codegen-escape" "see $ce_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$ce_log"
        fi
    fi
fi
