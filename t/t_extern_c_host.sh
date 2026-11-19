#!/bin/bash
# t_extern_c_host.sh — `extern func` bodies called from C and from Strada
#
# Delegates to t/extern_c_host_test/run.sh: builds a --shared library of
# extern functions that use strings, hashes and `our`, calls it from a C
# program linked with `strada --embed-ldflags` (valgrind-checked when
# available), calls the same functions from Strada (compiled and VM), and
# checks that extern bodies get semantic errors.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
ec_script="$SCRIPT_DIR/extern_c_host_test/run.sh"
if [ ! -x "$ec_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "extern-c-host" "runner not executable: $ec_script"
else
    ec_log="$BUILD_DIR/extern_c_host.log"
    if "$ec_script" > "$ec_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "extern-c-host (extern func bodies from C via --embed-ldflags, and from Strada)"
    else
        FAILED=$((FAILED + 1))
        log_fail "extern-c-host" "see $ec_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$ec_log"
        fi
    fi
fi
