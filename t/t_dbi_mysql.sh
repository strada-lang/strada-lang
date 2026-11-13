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

if ! command -v mysql_config >/dev/null 2>&1; then
    test_skip "DBI MySQL" "mysql_config not found (install libmysqlclient-dev)"
else
    _mysql_tmp_db=""
    _mysql_tmp_user=""
    _mysql_cleanup() {
        if [ -n "$_mysql_tmp_db" ]; then
            mysql -h 127.0.0.1 -P 3306 -e \
                "DROP DATABASE IF EXISTS \`$_mysql_tmp_db\`; DROP USER IF EXISTS '$_mysql_tmp_user'@'127.0.0.1'; DROP USER IF EXISTS '$_mysql_tmp_user'@'localhost';" \
                >/dev/null 2>&1 || true
            _mysql_tmp_db=""
            _mysql_tmp_user=""
        fi
    }

    _run_dsn="${STRADA_TEST_MYSQL_DSN:-}"
    _run_user="${STRADA_TEST_MYSQL_USER:-}"
    _run_pw="${STRADA_TEST_MYSQL_PASSWORD:-}"
    _provisioned=1

    if [ -z "$_run_dsn" ]; then
        _provisioned=0
        if command -v mysql >/dev/null 2>&1 && mysql -h 127.0.0.1 -P 3306 -N -B -e "SELECT 1" >/dev/null 2>&1; then
            _mysql_tmp_db="strada_test_dbi_$$"
            _mysql_tmp_user="strada_t_$$"
            _run_pw=$(od -An -tx1 -N12 /dev/urandom 2>/dev/null | tr -d ' \n')
            [ -z "$_run_pw" ] && _run_pw="strada_pw_$$"
            if mysql -h 127.0.0.1 -P 3306 >/dev/null 2>&1 <<SQL
CREATE DATABASE \`$_mysql_tmp_db\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS '$_mysql_tmp_user'@'127.0.0.1' IDENTIFIED BY '$_run_pw';
CREATE USER IF NOT EXISTS '$_mysql_tmp_user'@'localhost' IDENTIFIED BY '$_run_pw';
GRANT ALL PRIVILEGES ON \`$_mysql_tmp_db\`.* TO '$_mysql_tmp_user'@'127.0.0.1', '$_mysql_tmp_user'@'localhost';
FLUSH PRIVILEGES;
SQL
            then
                _run_dsn="dbi:mysql:database=$_mysql_tmp_db;host=127.0.0.1;port=3306"
                _run_user="$_mysql_tmp_user"
                _provisioned=1
            else
                _mysql_cleanup
            fi
        fi
    fi

    if [ "$_provisioned" -eq 0 ] || [ -z "$_run_dsn" ]; then
        test_skip "DBI MySQL" "no local MySQL access and STRADA_TEST_MYSQL_DSN unset"
    else
        trap _mysql_cleanup EXIT INT TERM
        TOTAL=$((TOTAL + 1))
        my_exe="$BUILD_DIR/test_dbi_mysql"
        my_log="$BUILD_DIR/test_dbi_mysql.log"
        if ! "$PROJECT_DIR/strada" --no-use-artifacts -DHAVE_MYSQL $(mysql_config --cflags) \
                "$PROJECT_DIR/lib/dbi/test_dbi_mysql.strada" $(mysql_config --libs) \
                -o "$my_exe" > "$my_log" 2>&1; then
            FAILED=$((FAILED + 1))
            log_fail "DBI MySQL" "compile failed, see $my_log"
        elif [ "${STRADA_MYSQL_VALGRIND:-0}" = "1" ] && command -v valgrind >/dev/null 2>&1; then
            if STRADA_TEST_MYSQL_DSN="$_run_dsn" STRADA_TEST_MYSQL_USER="$_run_user" STRADA_TEST_MYSQL_PASSWORD="$_run_pw" \
                    valgrind --leak-check=full --errors-for-leak-kinds=definite --error-exitcode=42 \
                    "$my_exe" >> "$my_log" 2>&1 && grep -q "PASS: All DBI MySQL tests passed" "$my_log"; then
                PASSED=$((PASSED + 1))
                log_pass "DBI MySQL (transactions, empty string vs NULL, binary data, valgrind 0-leak)"
            else
                FAILED=$((FAILED + 1))
                log_fail "DBI MySQL" "valgrind/test failure, see $my_log"
                [ "${VERBOSE:-0}" -eq 1 ] && cat "$my_log"
            fi
        elif STRADA_TEST_MYSQL_DSN="$_run_dsn" STRADA_TEST_MYSQL_USER="$_run_user" STRADA_TEST_MYSQL_PASSWORD="$_run_pw" \
                "$my_exe" >> "$my_log" 2>&1 && grep -q "PASS: All DBI MySQL tests passed" "$my_log"; then
            PASSED=$((PASSED + 1))
            log_pass "DBI MySQL (transactions, SELECT/migrator, DECIMAL, empty string vs NULL, binary data)"
        else
            FAILED=$((FAILED + 1))
            log_fail "DBI MySQL" "see $my_log"
            [ "${VERBOSE:-0}" -eq 1 ] && cat "$my_log"
        fi
        _mysql_cleanup
        trap - EXIT INT TERM
    fi
fi
