#!/bin/bash
# t_dbi_mysql.sh - DBI against a live MySQL/MariaDB server.
#
# Skipped unless STRADA_TEST_MYSQL_DSN is set (plus STRADA_TEST_MYSQL_USER /
# STRADA_TEST_MYSQL_PASSWORD), e.g.
#   STRADA_TEST_MYSQL_DSN="dbi:mysql:database=test;host=127.0.0.1;port=3306" \
#   STRADA_TEST_MYSQL_USER=me STRADA_TEST_MYSQL_PASSWORD=secret ./t/run_tests.sh
#
# The prebuilt lib/DBI.o is SQLite-only, so this compiles DBI from source with
# -DHAVE_MYSQL through the strada driver, the way MySQL applications build.

if [ -z "${STRADA_TEST_MYSQL_DSN:-}" ]; then
    test_skip "DBI MySQL" "set STRADA_TEST_MYSQL_DSN (+ _USER, _PASSWORD) to run"
elif ! command -v mysql_config >/dev/null 2>&1; then
    test_skip "DBI MySQL" "mysql_config not found (install libmysqlclient-dev)"
else
    TOTAL=$((TOTAL + 1))
    my_exe="$BUILD_DIR/test_dbi_mysql"
    my_log="$BUILD_DIR/test_dbi_mysql.log"
    if ! "$PROJECT_DIR/strada" --no-use-artifacts -DHAVE_MYSQL $(mysql_config --cflags) \
            "$PROJECT_DIR/lib/dbi/test_dbi_mysql.strada" $(mysql_config --libs) \
            -o "$my_exe" > "$my_log" 2>&1; then
        FAILED=$((FAILED + 1))
        log_fail "DBI MySQL" "compile failed, see $my_log"
    elif "$my_exe" >> "$my_log" 2>&1 && grep -q "PASS: All DBI MySQL tests passed" "$my_log"; then
        PASSED=$((PASSED + 1))
        log_pass "DBI MySQL (transactions, empty string vs NULL, binary data)"
    else
        FAILED=$((FAILED + 1))
        log_fail "DBI MySQL" "see $my_log"
        [ "${VERBOSE:-0}" -eq 1 ] && cat "$my_log"
    fi
fi
