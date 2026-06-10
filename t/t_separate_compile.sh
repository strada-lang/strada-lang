#!/bin/bash
# t_separate_compile.sh — separate-compilation tests
#
# Delegates to t/separate_compile_test/run.sh which exercises:
#   - `strada -M FILE` produces a sibling .o
#   - `-M` output is module-only (no transitively-inlined deps)
#   - `--object-full` still bundles deps (legacy behaviour)
#   - end-to-end: precompile + `use` auto-detects the .o
#   - stale .o (source newer than .o) falls back to source inlining
#   - `-M DIR` recurses
#   - reserved-type identifier produces a targeted error message
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
sc_script="$SCRIPT_DIR/separate_compile_test/run.sh"
if [ ! -x "$sc_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "separate-compile" "runner not executable: $sc_script"
else
    sc_log="$BUILD_DIR/separate_compile.log"
    if "$sc_script" > "$sc_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "separate-compile (-M, --object[-full], use auto-detect, reserved-type error)"
    else
        FAILED=$((FAILED + 1))
        log_fail "separate-compile" "see $sc_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$sc_log"
        fi
    fi
fi
