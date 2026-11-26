#!/bin/bash
# t_return_rules.sh — what a call yields follows Perl
#
# Delegates to t/return_rules_test/run.sh: return forms x caller contexts,
# compiled and through the VM (JIT on and off), against the output of the
# same programs in Perl.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
rr_script="$SCRIPT_DIR/return_rules_test/run.sh"
if [ ! -x "$rr_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "return-rules" "runner not executable: $rr_script"
else
    rr_log="$BUILD_DIR/return_rules.log"
    if "$rr_script" > "$rr_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "return-rules (what a call yields follows Perl: 778 cases, compiled and VM)"
    else
        FAILED=$((FAILED + 1))
        log_fail "return-rules" "see $rr_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$rr_log"
        fi
    fi
fi
