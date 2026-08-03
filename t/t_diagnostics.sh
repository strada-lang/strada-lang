#!/bin/bash
# t_diagnostics.sh — compiler diagnostics carry source locations
#
# Delegates to t/diagnostics_test/run.sh: lexer errors report file:line(:col),
# parse/lex/semantic errors inside use'd modules name the module's own path,
# and semantic errors/warnings report file:line. Counts the runner as a
# single pass/fail test.

TOTAL=$((TOTAL + 1))
dg_script="$SCRIPT_DIR/diagnostics_test/run.sh"
if [ ! -x "$dg_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "diagnostics" "runner not executable: $dg_script"
else
    dg_log="$BUILD_DIR/diagnostics.log"
    if "$dg_script" > "$dg_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "diagnostics (lexer/parser/semantic errors carry file:line, modules name their own file)"
    else
        FAILED=$((FAILED + 1))
        log_fail "diagnostics" "see $dg_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$dg_log"
        fi
    fi
fi
