#!/bin/bash
# t_shared_host_call.sh — --shared library calling a host-defined function
# (Cannoli report #4). Delegates to t/shared_host_call_test/run.sh.

TOTAL=$((TOTAL + 1))
sh_script="$SCRIPT_DIR/shared_host_call_test/run.sh"
sh_log="$BUILD_DIR/shared_host_call.log"
if "$sh_script" > "$sh_log" 2>&1; then
    PASSED=$((PASSED + 1))
    log_pass "shared-host-call (--shared plugin calling a host function)"
else
    FAILED=$((FAILED + 1))
    log_fail "shared-host-call" "see $sh_log"
    if [ $VERBOSE -eq 1 ]; then cat "$sh_log"; fi
fi
