/*
 * strada_dbi.h - Strada Database Interface
 *
 * Provides a unified interface for database operations similar to Perl's DBI.
 * Supports SQLite, MySQL/MariaDB, and PostgreSQL.
 */

#ifndef STRADA_DBI_H
#define STRADA_DBI_H

#include "strada_runtime.h"
#include <stdint.h>

/* Database handle types */
typedef enum {
    DBI_DRIVER_NONE = 0,
    DBI_DRIVER_SQLITE,
    DBI_DRIVER_MYSQL,
    DBI_DRIVER_POSTGRES
} DbiDriverType;

/* Forward declarations */
typedef struct DbiHandle DbiHandle;
typedef struct DbiStatement DbiStatement;

/* Database handle structure */
struct DbiHandle {
    DbiDriverType driver;
    void *conn;                 /* Native connection handle */
    char *dsn;                  /* Data source name */
    char *username;
    char *error_msg;
    int error_code;
    int auto_commit;
    int in_transaction;
    int raise_error;
    int print_error;
    int connected;
};

/* Statement handle structure */
struct DbiStatement {
    DbiHandle *dbh;
    void *stmt;                 /* Native statement handle */
    char *sql;
    int num_params;
    int num_columns;
    char **column_names;
    int *column_types;
    int executed;
    int finished;
    void *result;               /* For MySQL result sets */
    int row_count;
    int affected_rows;
};

/* Connection functions */
DbiHandle* dbi_connect(const char *dsn, const char *username, const char *password, StradaValue *attrs);
void dbi_disconnect(DbiHandle *dbh);
int dbi_ping(DbiHandle *dbh);

/* Statement functions */
DbiStatement* dbi_prepare(DbiHandle *dbh, const char *sql);
int dbi_execute(DbiStatement *sth, StradaValue *params);
int dbi_do(DbiHandle *dbh, const char *sql, StradaValue *params);

/* Fetch functions */
StradaValue* dbi_fetchrow_array(DbiStatement *sth);
StradaValue* dbi_fetchrow_hashref(DbiStatement *sth);
StradaValue* dbi_fetchrow_arrayref(DbiStatement *sth);
StradaValue* dbi_fetchall_arrayref(DbiStatement *sth);
StradaValue* dbi_fetchall_hashref(DbiStatement *sth, const char *key_field);

/* Result info */
int dbi_rows(DbiStatement *sth);
StradaValue* dbi_column_names(DbiStatement *sth);
StradaValue* dbi_column_types(DbiStatement *sth);

/* Transaction functions */
int dbi_begin_work(DbiHandle *dbh);
int dbi_commit(DbiHandle *dbh);
int dbi_rollback(DbiHandle *dbh);

/* Utility functions */
char* dbi_quote(DbiHandle *dbh, const char *str);
char* dbi_quote_identifier(DbiHandle *dbh, const char *identifier);
int64_t dbi_last_insert_id(DbiHandle *dbh, const char *catalog, const char *schema, const char *table, const char *field);

/* Error handling */
const char* dbi_errstr(DbiHandle *dbh);
int dbi_err(DbiHandle *dbh);
const char* dbi_state(DbiHandle *dbh);

/* Statement cleanup */
void dbi_finish(DbiStatement *sth);
void dbi_free_statement(DbiStatement *sth);

/* Bind functions */
int dbi_bind_param(DbiStatement *sth, int param_num, StradaValue *value);
int dbi_bind_columns(DbiStatement *sth, StradaValue *refs);

/* Attribute access */
StradaValue* dbi_get_attr(DbiHandle *dbh, const char *attr);
int dbi_set_attr(DbiHandle *dbh, const char *attr, StradaValue *value);

/* Strada interface functions */
StradaValue* strada_dbi_connect(StradaValue *dsn, StradaValue *user, StradaValue *pass, StradaValue *attrs);
void strada_dbi_disconnect(StradaValue *dbh);
StradaValue* strada_dbi_prepare(StradaValue *dbh, StradaValue *sql);
StradaValue* strada_dbi_execute(StradaValue *sth, StradaValue *params);
StradaValue* strada_dbi_do(StradaValue *dbh, StradaValue *sql, StradaValue *params);
StradaValue* strada_dbi_fetchrow_array(StradaValue *sth);
StradaValue* strada_dbi_fetchrow_hashref(StradaValue *sth);
StradaValue* strada_dbi_fetchall_arrayref(StradaValue *sth);
StradaValue* strada_dbi_begin_work(StradaValue *dbh);
StradaValue* strada_dbi_commit(StradaValue *dbh);
StradaValue* strada_dbi_rollback(StradaValue *dbh);
StradaValue* strada_dbi_quote(StradaValue *dbh, StradaValue *str);
StradaValue* strada_dbi_last_insert_id(StradaValue *dbh);
StradaValue* strada_dbi_errstr(StradaValue *dbh);
StradaValue* strada_dbi_err(StradaValue *dbh);
void strada_dbi_finish(StradaValue *sth);
StradaValue* strada_dbi_rows(StradaValue *sth);
StradaValue* strada_dbi_column_names(StradaValue *sth);
StradaValue* strada_dbi_ping(StradaValue *dbh);

/* ============== Raw C Functions for extern "C" ============== */
/* These use raw C types instead of StradaValue* for direct FFI usage */

/* Connection */
DbiHandle* strada_dbi_connect_raw(const char *dsn, const char *user, const char *pass,
                                   int auto_commit, int print_error);
void strada_dbi_disconnect_raw(DbiHandle *dbh);
int strada_dbi_ping_raw(DbiHandle *dbh);

/* Statement */
DbiStatement* strada_dbi_prepare_raw(DbiHandle *dbh, const char *sql);
int strada_dbi_execute_raw(DbiStatement *sth);
int strada_dbi_do_raw(DbiHandle *dbh, const char *sql);
void strada_dbi_finish_raw(DbiStatement *sth);
void strada_dbi_free_statement_raw(DbiStatement *sth);
int strada_dbi_num_params(DbiStatement *sth);

/* Bind parameters */
void strada_dbi_bind_int(DbiStatement *sth, int idx, int64_t val);
void strada_dbi_bind_str(DbiStatement *sth, int idx, const char *val);
void strada_dbi_bind_num(DbiStatement *sth, int idx, double val);
void strada_dbi_bind_null(DbiStatement *sth, int idx);
void strada_dbi_clear_bindings(DbiStatement *sth);

/* Step-based fetch */
int strada_dbi_step(DbiStatement *sth);

/* Column accessors */
int strada_dbi_column_count(DbiStatement *sth);
char* strada_dbi_column_name(DbiStatement *sth, int idx);
int strada_dbi_column_type(DbiStatement *sth, int idx);
int64_t strada_dbi_column_int(DbiStatement *sth, int idx);
double strada_dbi_column_num(DbiStatement *sth, int idx);
char* strada_dbi_column_str(DbiStatement *sth, int idx);
int strada_dbi_column_is_null(DbiStatement *sth, int idx);

/* Transactions */
int strada_dbi_begin_work_raw(DbiHandle *dbh);
int strada_dbi_commit_raw(DbiHandle *dbh);
int strada_dbi_rollback_raw(DbiHandle *dbh);

/* Utility */
char* strada_dbi_quote_raw(DbiHandle *dbh, const char *str);
int64_t strada_dbi_last_insert_id_raw(DbiHandle *dbh);
const char* strada_dbi_errstr_raw(DbiHandle *dbh);
int strada_dbi_err_raw(DbiHandle *dbh);
int strada_dbi_rows_raw(DbiStatement *sth);

#endif /* STRADA_DBI_H */
