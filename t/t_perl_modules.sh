#!/bin/bash
# t_perl_modules.sh — Perl-compatible library modules
#
# Delegates to t/perl_modules_test/run.sh: Getopt::Long, File::Basename,
# File::Temp, File::Spec, Cwd, File::Path, File::Copy, Carp and Scalar::Util
# programs, compiled and through the VM, against expected output that was
# checked against Perl's modules.
#
# Counts the runner as a single test that either passes or fails.

TOTAL=$((TOTAL + 1))
pm_script="$SCRIPT_DIR/perl_modules_test/run.sh"
if [ ! -x "$pm_script" ]; then
    FAILED=$((FAILED + 1))
    log_fail "perl-modules" "runner not executable: $pm_script"
else
    pm_log="$BUILD_DIR/perl_modules.log"
    if "$pm_script" > "$pm_log" 2>&1; then
        PASSED=$((PASSED + 1))
        log_pass "perl-modules (Getopt::Long, File::Basename/Temp/Spec/Path/Copy, Cwd, Carp, Scalar::Util; compiled and VM)"
    else
        FAILED=$((FAILED + 1))
        log_fail "perl-modules" "see $pm_log"
        if [ $VERBOSE -eq 1 ]; then
            cat "$pm_log"
        fi
    fi
fi
