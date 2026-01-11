/*
 This file is part of the Strada Language (https://github.com/mjflick/strada-lang).
 Copyright (c) 2026 Michael J. Flickinger
 
 This program is free software: you can redistribute it and/or modify  
 it under the terms of the GNU General Public License as published by  
 the Free Software Foundation, version 2.

 This program is distributed in the hope that it will be useful, but 
 WITHOUT ANY WARRANTY; without even the implied warranty of 
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 General Public License for more details.

 You should have received a copy of the GNU General Public License 
 along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * strada_dbi.c - Strada Database Interface Implementation
 *
 * Supports SQLite, MySQL/MariaDB, and PostgreSQL through a unified API.
 */

#define _GNU_SOURCE
#include "strada_dbi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Conditional includes for database libraries */
#ifdef HAVE_SQLITE3
#include <sqlite3.h>
#endif

#ifdef HAVE_MYSQL
#include <mysql/mysql.h>
#endif

#ifdef HAVE_POSTGRES
#include <libpq-fe.h>
#endif

/* Always include SQLite as it's commonly available */
#ifndef HAVE_SQLITE3
#define HAVE_SQLITE3 1
#include <sqlite3.h>
#endif

/* Helper: Parse DSN string */
static DbiDriverType parse_dsn(const char *dsn, char **database, char **host, int *port) {
    *database = NULL;
    *host = NULL;
    *port = 0;

    if (!dsn) return DBI_DRIVER_NONE;

    /* DSN format: dbi:Driver:database=name;host=hostname;port=number */
    if (strncmp(dsn, "dbi:", 4) != 0 && strncmp(dsn, "DBI:", 4) != 0) {
        /* Try simple format: driver:database */
        if (strncmp(dsn, "sqlite:", 7) == 0 || strncmp(dsn, "SQLite:", 7) == 0) {
            *database = strdup(dsn + 7);
            return DBI_DRIVER_SQLITE;
        }
        return DBI_DRIVER_NONE;
    }

    const char *p = dsn + 4;  /* Skip "dbi:" */

    /* Determine driver */
    DbiDriverType driver = DBI_DRIVER_NONE;
    if (strncasecmp(p, "sqlite:", 7) == 0) {
        driver = DBI_DRIVER_SQLITE;
        p += 7;
    } else if (strncasecmp(p, "mysql:", 6) == 0) {
        driver = DBI_DRIVER_MYSQL;
        p += 6;
    } else if (strncasecmp(p, "pg:", 3) == 0 || strncasecmp(p, "postgres:", 9) == 0) {
        driver = DBI_DRIVER_POSTGRES;
        p += (strncasecmp(p, "pg:", 3) == 0) ? 3 : 9;
    }

    if (driver == DBI_DRIVER_NONE) return DBI_DRIVER_NONE;

    /* Parse connection parameters */
    char *params = strdup(p);
    char *saveptr;
    char *token = strtok_r(params, ";", &saveptr);

    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            char *key = token;
            char *value = eq + 1;

            /* Trim whitespace */
            while (*key && isspace(*key)) key++;
            while (*value && isspace(*value)) value++;

            if (strcasecmp(key, "database") == 0 || strcasecmp(key, "dbname") == 0) {
                *database = strdup(value);
            } else if (strcasecmp(key, "host") == 0) {
                *host = strdup(value);
            } else if (strcasecmp(key, "port") == 0) {
                *port = atoi(value);
            }
        } else {
            /* Might be just the database name */
            if (!*database) {
                *database = strdup(token);
            }
        }
        token = strtok_r(NULL, ";", &saveptr);
    }

    free(params);
    return driver;
}

/* Helper: Set error */
static void set_error(DbiHandle *dbh, int code, const char *msg) {
    if (dbh->error_msg) free(dbh->error_msg);
    dbh->error_code = code;
    dbh->error_msg = msg ? strdup(msg) : NULL;

    if (dbh->print_error && msg) {
        fprintf(stderr, "DBI Error: %s\n", msg);
    }
}

/* ============== Connection Functions ============== */

DbiHandle* dbi_connect(const char *dsn, const char *username, const char *password, StradaValue *attrs) {
    DbiHandle *dbh = calloc(1, sizeof(DbiHandle));
    if (!dbh) return NULL;

    dbh->auto_commit = 1;
    dbh->raise_error = 0;
    dbh->print_error = 1;

    /* Parse attributes */
    if (attrs && attrs->type == STRADA_HASH) {
        StradaValue *ac = strada_hash_get(attrs->value.hv, "AutoCommit");
        if (ac) dbh->auto_commit = strada_to_int(ac);

        StradaValue *re = strada_hash_get(attrs->value.hv, "RaiseError");
        if (re) dbh->raise_error = strada_to_int(re);

        StradaValue *pe = strada_hash_get(attrs->value.hv, "PrintError");
        if (pe) dbh->print_error = strada_to_int(pe);
    }

    char *database = NULL;
    char *host = NULL;
    int port = 0;

    dbh->driver = parse_dsn(dsn, &database, &host, &port);
    dbh->dsn = dsn ? strdup(dsn) : NULL;
    dbh->username = username ? strdup(username) : NULL;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3 *db;
            int rc = sqlite3_open(database ? database : ":memory:", &db);
            if (rc != SQLITE_OK) {
                set_error(dbh, rc, sqlite3_errmsg(db));
                sqlite3_close(db);
                free(database);
                free(host);
                free(dbh);
                return NULL;
            }
            dbh->conn = db;
            dbh->connected = 1;
            break;
        }
#endif

#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL: {
            MYSQL *mysql = mysql_init(NULL);
            if (!mysql) {
                set_error(dbh, -1, "MySQL initialization failed");
                free(database);
                free(host);
                free(dbh);
                return NULL;
            }

            if (!mysql_real_connect(mysql,
                                   host ? host : "localhost",
                                   username,
                                   password,
                                   database,
                                   port ? port : 3306,
                                   NULL, 0)) {
                set_error(dbh, mysql_errno(mysql), mysql_error(mysql));
                mysql_close(mysql);
                free(database);
                free(host);
                free(dbh);
                return NULL;
            }

            if (dbh->auto_commit) {
                mysql_autocommit(mysql, 1);
            }

            dbh->conn = mysql;
            dbh->connected = 1;
            break;
        }
#endif

#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            char conninfo[1024];
            snprintf(conninfo, sizeof(conninfo),
                    "host=%s port=%d dbname=%s user=%s password=%s",
                    host ? host : "localhost",
                    port ? port : 5432,
                    database ? database : "",
                    username ? username : "",
                    password ? password : "");

            PGconn *pg = PQconnectdb(conninfo);
            if (PQstatus(pg) != CONNECTION_OK) {
                set_error(dbh, -1, PQerrorMessage(pg));
                PQfinish(pg);
                free(database);
                free(host);
                free(dbh);
                return NULL;
            }

            dbh->conn = pg;
            dbh->connected = 1;
            break;
        }
#endif

        default:
            set_error(dbh, -1, "Unknown or unsupported database driver");
            free(database);
            free(host);
            free(dbh);
            return NULL;
    }

    free(database);
    free(host);
    return dbh;
}

void dbi_disconnect(DbiHandle *dbh) {
    if (!dbh) return;

    if (dbh->in_transaction) {
        dbi_rollback(dbh);
    }

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            if (dbh->conn) sqlite3_close((sqlite3*)dbh->conn);
            break;
#endif
#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL:
            if (dbh->conn) mysql_close((MYSQL*)dbh->conn);
            break;
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES:
            if (dbh->conn) PQfinish((PGconn*)dbh->conn);
            break;
#endif
        default:
            break;
    }

    free(dbh->dsn);
    free(dbh->username);
    free(dbh->error_msg);
    free(dbh);
}

int dbi_ping(DbiHandle *dbh) {
    if (!dbh || !dbh->connected) return 0;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            return 1;  /* SQLite is always "connected" */
#endif
#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL:
            return mysql_ping((MYSQL*)dbh->conn) == 0;
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES:
            return PQstatus((PGconn*)dbh->conn) == CONNECTION_OK;
#endif
        default:
            return 0;
    }
}

/* ============== Statement Functions ============== */

DbiStatement* dbi_prepare(DbiHandle *dbh, const char *sql) {
    if (!dbh || !dbh->connected || !sql) return NULL;

    DbiStatement *sth = calloc(1, sizeof(DbiStatement));
    if (!sth) return NULL;

    sth->dbh = dbh;
    sth->sql = strdup(sql);

    /* Count placeholders (?) */
    const char *p = sql;
    while ((p = strchr(p, '?')) != NULL) {
        sth->num_params++;
        p++;
    }

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3_stmt *stmt;
            int rc = sqlite3_prepare_v2((sqlite3*)dbh->conn, sql, -1, &stmt, NULL);
            if (rc != SQLITE_OK) {
                set_error(dbh, rc, sqlite3_errmsg((sqlite3*)dbh->conn));
                free(sth->sql);
                free(sth);
                return NULL;
            }
            sth->stmt = stmt;
            sth->num_columns = sqlite3_column_count(stmt);

            /* Get column names */
            if (sth->num_columns > 0) {
                sth->column_names = calloc(sth->num_columns, sizeof(char*));
                for (int i = 0; i < sth->num_columns; i++) {
                    const char *name = sqlite3_column_name(stmt, i);
                    sth->column_names[i] = name ? strdup(name) : strdup("");
                }
            }
            break;
        }
#endif

#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL: {
            MYSQL_STMT *stmt = mysql_stmt_init((MYSQL*)dbh->conn);
            if (!stmt) {
                set_error(dbh, mysql_errno((MYSQL*)dbh->conn), mysql_error((MYSQL*)dbh->conn));
                free(sth->sql);
                free(sth);
                return NULL;
            }

            if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
                set_error(dbh, mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
                mysql_stmt_close(stmt);
                free(sth->sql);
                free(sth);
                return NULL;
            }

            sth->stmt = stmt;
            sth->num_params = mysql_stmt_param_count(stmt);

            MYSQL_RES *meta = mysql_stmt_result_metadata(stmt);
            if (meta) {
                sth->num_columns = mysql_num_fields(meta);
                sth->column_names = calloc(sth->num_columns, sizeof(char*));
                MYSQL_FIELD *fields = mysql_fetch_fields(meta);
                for (int i = 0; i < sth->num_columns; i++) {
                    sth->column_names[i] = strdup(fields[i].name);
                }
                mysql_free_result(meta);
            }
            break;
        }
#endif

#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES:
            /* PostgreSQL uses $1, $2 style placeholders */
            /* We'll execute with PQexecParams later */
            sth->stmt = NULL;  /* No prepared statement handle needed */
            break;
#endif

        default:
            free(sth->sql);
            free(sth);
            return NULL;
    }

    return sth;
}

int dbi_execute(DbiStatement *sth, StradaValue *params) {
    if (!sth || !sth->dbh) return -1;

    DbiHandle *dbh = sth->dbh;
    sth->executed = 1;
    sth->finished = 0;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3_stmt *stmt = (sqlite3_stmt*)sth->stmt;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);

            /* Bind parameters */
            if (params && params->type == STRADA_ARRAY) {
                StradaArray *arr = params->value.av;
                for (size_t i = 0; i < arr->size && i < (size_t)sth->num_params; i++) {
                    StradaValue *val = arr->elements[i];
                    if (!val || val->type == STRADA_UNDEF) {
                        sqlite3_bind_null(stmt, i + 1);
                    } else if (val->type == STRADA_INT) {
                        sqlite3_bind_int64(stmt, i + 1, val->value.iv);
                    } else if (val->type == STRADA_NUM) {
                        sqlite3_bind_double(stmt, i + 1, val->value.nv);
                    } else {
                        const char *str = strada_to_str(val);
                        sqlite3_bind_text(stmt, i + 1, str, -1, SQLITE_TRANSIENT);
                    }
                }
            }

            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) {
                sth->affected_rows = sqlite3_changes((sqlite3*)dbh->conn);
                sth->finished = 1;
                return sth->affected_rows;
            } else if (rc == SQLITE_ROW) {
                /* SELECT query - reset to allow fetching */
                sqlite3_reset(stmt);
                /* Re-bind and step will happen in fetch */
                if (params && params->type == STRADA_ARRAY) {
                    StradaArray *arr = params->value.av;
                    for (size_t i = 0; i < arr->size && i < (size_t)sth->num_params; i++) {
                        StradaValue *val = arr->elements[i];
                        if (!val || val->type == STRADA_UNDEF) {
                            sqlite3_bind_null(stmt, i + 1);
                        } else if (val->type == STRADA_INT) {
                            sqlite3_bind_int64(stmt, i + 1, val->value.iv);
                        } else if (val->type == STRADA_NUM) {
                            sqlite3_bind_double(stmt, i + 1, val->value.nv);
                        } else {
                            const char *str = strada_to_str(val);
                            sqlite3_bind_text(stmt, i + 1, str, -1, SQLITE_TRANSIENT);
                        }
                    }
                }
                return 0;  /* 0 affected rows for SELECT, but success */
            } else {
                set_error(dbh, rc, sqlite3_errmsg((sqlite3*)dbh->conn));
                return -1;
            }
        }
#endif

#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL: {
            MYSQL_STMT *stmt = (MYSQL_STMT*)sth->stmt;

            /* Bind parameters */
            MYSQL_BIND *binds = NULL;
            if (sth->num_params > 0 && params && params->type == STRADA_ARRAY) {
                binds = calloc(sth->num_params, sizeof(MYSQL_BIND));
                StradaArray *arr = params->value.av;

                for (int i = 0; i < sth->num_params && i < (int)arr->size; i++) {
                    StradaValue *val = arr->elements[i];
                    if (!val || val->type == STRADA_UNDEF) {
                        binds[i].buffer_type = MYSQL_TYPE_NULL;
                    } else if (val->type == STRADA_INT) {
                        binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                        binds[i].buffer = &val->value.iv;
                    } else if (val->type == STRADA_NUM) {
                        binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                        binds[i].buffer = &val->value.nv;
                    } else {
                        const char *str = strada_to_str(val);
                        binds[i].buffer_type = MYSQL_TYPE_STRING;
                        binds[i].buffer = (void*)str;
                        binds[i].buffer_length = strlen(str);
                    }
                }

                if (mysql_stmt_bind_param(stmt, binds) != 0) {
                    set_error(dbh, mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
                    free(binds);
                    return -1;
                }
            }

            if (mysql_stmt_execute(stmt) != 0) {
                set_error(dbh, mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
                free(binds);
                return -1;
            }

            sth->affected_rows = mysql_stmt_affected_rows(stmt);
            free(binds);
            return sth->affected_rows;
        }
#endif

#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            /* Convert ? placeholders to $1, $2, etc. */
            char *converted_sql = strdup(sth->sql);
            int param_num = 1;
            char *p = converted_sql;
            size_t new_len = strlen(converted_sql) + sth->num_params * 10;
            char *new_sql = malloc(new_len);
            char *np = new_sql;

            while (*p) {
                if (*p == '?') {
                    np += sprintf(np, "$%d", param_num++);
                } else {
                    *np++ = *p;
                }
                p++;
            }
            *np = '\0';

            /* Prepare parameter values */
            char **param_values = NULL;
            if (sth->num_params > 0 && params && params->type == STRADA_ARRAY) {
                param_values = calloc(sth->num_params, sizeof(char*));
                StradaArray *arr = params->value.av;

                for (int i = 0; i < sth->num_params && i < (int)arr->size; i++) {
                    StradaValue *val = arr->elements[i];
                    if (val && val->type != STRADA_UNDEF) {
                        param_values[i] = strdup(strada_to_str(val));
                    }
                }
            }

            PGresult *res = PQexecParams((PGconn*)dbh->conn, new_sql,
                                        sth->num_params, NULL,
                                        (const char* const*)param_values,
                                        NULL, NULL, 0);

            free(new_sql);
            free(converted_sql);

            ExecStatusType status = PQresultStatus(res);
            if (status == PGRES_COMMAND_OK) {
                char *rows = PQcmdTuples(res);
                sth->affected_rows = rows ? atoi(rows) : 0;
                sth->finished = 1;
            } else if (status == PGRES_TUPLES_OK) {
                sth->result = res;
                sth->row_count = PQntuples(res);
                sth->num_columns = PQnfields(res);

                /* Get column names */
                if (sth->num_columns > 0) {
                    sth->column_names = calloc(sth->num_columns, sizeof(char*));
                    for (int i = 0; i < sth->num_columns; i++) {
                        sth->column_names[i] = strdup(PQfname(res, i));
                    }
                }
                sth->affected_rows = 0;
            } else {
                set_error(dbh, -1, PQresultErrorMessage(res));
                PQclear(res);

                /* Free param values */
                if (param_values) {
                    for (int i = 0; i < sth->num_params; i++) {
                        free(param_values[i]);
                    }
                    free(param_values);
                }
                return -1;
            }

            /* Free param values */
            if (param_values) {
                for (int i = 0; i < sth->num_params; i++) {
                    free(param_values[i]);
                }
                free(param_values);
            }

            return sth->affected_rows;
        }
#endif

        default:
            return -1;
    }
}

int dbi_do(DbiHandle *dbh, const char *sql, StradaValue *params) {
    DbiStatement *sth = dbi_prepare(dbh, sql);
    if (!sth) return -1;

    int result = dbi_execute(sth, params);
    dbi_free_statement(sth);
    return result;
}

/* ============== Fetch Functions ============== */

StradaValue* dbi_fetchrow_array(DbiStatement *sth) {
    if (!sth || !sth->executed || sth->finished) return NULL;

    DbiHandle *dbh = sth->dbh;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3_stmt *stmt = (sqlite3_stmt*)sth->stmt;
            int rc = sqlite3_step(stmt);

            if (rc == SQLITE_ROW) {
                StradaValue *arr = strada_new_array();
                int cols = sqlite3_column_count(stmt);

                for (int i = 0; i < cols; i++) {
                    int type = sqlite3_column_type(stmt, i);
                    StradaValue *val;

                    switch (type) {
                        case SQLITE_NULL:
                            val = strada_new_undef();
                            break;
                        case SQLITE_INTEGER:
                            val = strada_new_int(sqlite3_column_int64(stmt, i));
                            break;
                        case SQLITE_FLOAT:
                            val = strada_new_num(sqlite3_column_double(stmt, i));
                            break;
                        default:
                            val = strada_new_str((const char*)sqlite3_column_text(stmt, i));
                            break;
                    }
                    strada_array_push(arr->value.av, val);
                }
                return arr;
            } else {
                sth->finished = 1;
                return NULL;
            }
        }
#endif

#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL: {
            MYSQL_STMT *stmt = (MYSQL_STMT*)sth->stmt;

            if (mysql_stmt_fetch(stmt) == 0) {
                /* Need to set up result bindings first */
                /* Simplified: return NULL for now */
                return NULL;
            }
            sth->finished = 1;
            return NULL;
        }
#endif

#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            static int pg_row = 0;
            if (!sth->result) return NULL;

            PGresult *res = (PGresult*)sth->result;
            if (pg_row >= PQntuples(res)) {
                pg_row = 0;
                sth->finished = 1;
                return NULL;
            }

            StradaValue *arr = strada_new_array();
            int cols = PQnfields(res);

            for (int i = 0; i < cols; i++) {
                StradaValue *val;
                if (PQgetisnull(res, pg_row, i)) {
                    val = strada_new_undef();
                } else {
                    val = strada_new_str(PQgetvalue(res, pg_row, i));
                }
                strada_array_push(arr->value.av, val);
            }

            pg_row++;
            return arr;
        }
#endif

        default:
            return NULL;
    }
}

StradaValue* dbi_fetchrow_hashref(DbiStatement *sth) {
    if (!sth || !sth->executed || sth->finished) return NULL;

    DbiHandle *dbh = sth->dbh;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3_stmt *stmt = (sqlite3_stmt*)sth->stmt;
            int rc = sqlite3_step(stmt);

            if (rc == SQLITE_ROW) {
                StradaValue *hash = strada_new_hash();
                int cols = sqlite3_column_count(stmt);

                for (int i = 0; i < cols; i++) {
                    const char *name = sqlite3_column_name(stmt, i);
                    int type = sqlite3_column_type(stmt, i);
                    StradaValue *val;

                    switch (type) {
                        case SQLITE_NULL:
                            val = strada_new_undef();
                            break;
                        case SQLITE_INTEGER:
                            val = strada_new_int(sqlite3_column_int64(stmt, i));
                            break;
                        case SQLITE_FLOAT:
                            val = strada_new_num(sqlite3_column_double(stmt, i));
                            break;
                        default:
                            val = strada_new_str((const char*)sqlite3_column_text(stmt, i));
                            break;
                    }
                    strada_hash_set(hash->value.hv, name, val);
                }
                return hash;
            } else {
                sth->finished = 1;
                return NULL;
            }
        }
#endif

#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            static int pg_row_hash = 0;
            if (!sth->result) return NULL;

            PGresult *res = (PGresult*)sth->result;
            if (pg_row_hash >= PQntuples(res)) {
                pg_row_hash = 0;
                sth->finished = 1;
                return NULL;
            }

            StradaValue *hash = strada_new_hash();
            int cols = PQnfields(res);

            for (int i = 0; i < cols; i++) {
                const char *name = PQfname(res, i);
                StradaValue *val;
                if (PQgetisnull(res, pg_row_hash, i)) {
                    val = strada_new_undef();
                } else {
                    val = strada_new_str(PQgetvalue(res, pg_row_hash, i));
                }
                strada_hash_set(hash->value.hv, name, val);
            }

            pg_row_hash++;
            return hash;
        }
#endif

        default:
            return NULL;
    }
}

StradaValue* dbi_fetchall_arrayref(DbiStatement *sth) {
    StradaValue *all = strada_new_array();
    StradaValue *row;

    while ((row = dbi_fetchrow_array(sth)) != NULL) {
        strada_array_push(all->value.av, row);
    }

    return all;
}

/* ============== Transaction Functions ============== */

int dbi_begin_work(DbiHandle *dbh) {
    if (!dbh || !dbh->connected) return -1;

    if (dbh->in_transaction) {
        set_error(dbh, -1, "Already in a transaction");
        return -1;
    }

    int rc = dbi_do(dbh, "BEGIN", NULL);
    if (rc >= 0) {
        dbh->in_transaction = 1;
    }
    return rc;
}

int dbi_commit(DbiHandle *dbh) {
    if (!dbh || !dbh->connected) return -1;

    int rc = dbi_do(dbh, "COMMIT", NULL);
    dbh->in_transaction = 0;
    return rc;
}

int dbi_rollback(DbiHandle *dbh) {
    if (!dbh || !dbh->connected) return -1;

    int rc = dbi_do(dbh, "ROLLBACK", NULL);
    dbh->in_transaction = 0;
    return rc;
}

/* ============== Utility Functions ============== */

char* dbi_quote(DbiHandle *dbh, const char *str) {
    if (!str) return strdup("NULL");

    size_t len = strlen(str);
    char *quoted = malloc(len * 2 + 3);
    char *p = quoted;

    *p++ = '\'';
    while (*str) {
        if (*str == '\'') {
            *p++ = '\'';
            *p++ = '\'';
        } else if (*str == '\\' && dbh->driver == DBI_DRIVER_MYSQL) {
            *p++ = '\\';
            *p++ = '\\';
        } else {
            *p++ = *str;
        }
        str++;
    }
    *p++ = '\'';
    *p = '\0';

    return quoted;
}

int64_t dbi_last_insert_id(DbiHandle *dbh, const char *catalog, const char *schema,
                           const char *table, const char *field) {
    (void)catalog; (void)schema; (void)table; (void)field;

    if (!dbh || !dbh->connected) return -1;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            return sqlite3_last_insert_rowid((sqlite3*)dbh->conn);
#endif
#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL:
            return mysql_insert_id((MYSQL*)dbh->conn);
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            PGresult *res = PQexec((PGconn*)dbh->conn, "SELECT lastval()");
            if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                int64_t id = strtoll(PQgetvalue(res, 0, 0), NULL, 10);
                PQclear(res);
                return id;
            }
            PQclear(res);
            return -1;
        }
#endif
        default:
            return -1;
    }
}

/* ============== Error Handling ============== */

const char* dbi_errstr(DbiHandle *dbh) {
    if (!dbh) return "No database handle";
    return dbh->error_msg ? dbh->error_msg : "";
}

int dbi_err(DbiHandle *dbh) {
    if (!dbh) return -1;
    return dbh->error_code;
}

/* ============== Statement Cleanup ============== */

void dbi_finish(DbiStatement *sth) {
    if (!sth) return;
    sth->finished = 1;

#ifdef HAVE_SQLITE3
    if (sth->dbh->driver == DBI_DRIVER_SQLITE && sth->stmt) {
        sqlite3_reset((sqlite3_stmt*)sth->stmt);
    }
#endif
}

void dbi_free_statement(DbiStatement *sth) {
    if (!sth) return;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            if (sth->stmt) sqlite3_finalize((sqlite3_stmt*)sth->stmt);
            break;
#endif
#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL:
            if (sth->stmt) mysql_stmt_close((MYSQL_STMT*)sth->stmt);
            break;
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES:
            if (sth->result) PQclear((PGresult*)sth->result);
            break;
#endif
        default:
            break;
    }

    free(sth->sql);
    if (sth->column_names) {
        for (int i = 0; i < sth->num_columns; i++) {
            free(sth->column_names[i]);
        }
        free(sth->column_names);
    }
    free(sth->column_types);
    free(sth);
}

int dbi_rows(DbiStatement *sth) {
    if (!sth) return -1;
    return sth->affected_rows;
}

StradaValue* dbi_column_names(DbiStatement *sth) {
    if (!sth || !sth->column_names) return strada_new_array();

    StradaValue *arr = strada_new_array();
    for (int i = 0; i < sth->num_columns; i++) {
        strada_array_push(arr->value.av, strada_new_str(sth->column_names[i]));
    }
    return arr;
}

/* ============== Strada Interface Functions ============== */

/* Global handle storage (simple implementation) */
static DbiHandle *g_handles[256];
static int g_handle_count = 0;
static DbiStatement *g_stmts[1024];
static int g_stmt_count = 0;

StradaValue* strada_dbi_connect(StradaValue *dsn, StradaValue *user, StradaValue *pass, StradaValue *attrs) {
    const char *dsn_str = dsn ? strada_to_str(dsn) : NULL;
    const char *user_str = user ? strada_to_str(user) : NULL;
    const char *pass_str = pass ? strada_to_str(pass) : NULL;

    DbiHandle *dbh = dbi_connect(dsn_str, user_str, pass_str, attrs);
    if (!dbh) return strada_new_undef();

    /* Store handle and return index */
    int idx = g_handle_count++;
    g_handles[idx] = dbh;

    /* Return as hash with handle info */
    StradaValue *result = strada_new_hash();
    strada_hash_set(result->value.hv, "_handle_id", strada_new_int(idx));
    strada_hash_set(result->value.hv, "Driver", strada_new_str(
        dbh->driver == DBI_DRIVER_SQLITE ? "SQLite" :
        dbh->driver == DBI_DRIVER_MYSQL ? "MySQL" :
        dbh->driver == DBI_DRIVER_POSTGRES ? "PostgreSQL" : "Unknown"));
    strada_hash_set(result->value.hv, "AutoCommit", strada_new_int(dbh->auto_commit));

    return result;
}

static DbiHandle* get_dbh(StradaValue *sv) {
    if (!sv || sv->type != STRADA_HASH) return NULL;
    StradaValue *id = strada_hash_get(sv->value.hv, "_handle_id");
    if (!id) return NULL;
    int idx = strada_to_int(id);
    if (idx < 0 || idx >= g_handle_count) return NULL;
    return g_handles[idx];
}

static DbiStatement* get_sth(StradaValue *sv) {
    if (!sv || sv->type != STRADA_HASH) return NULL;
    StradaValue *id = strada_hash_get(sv->value.hv, "_stmt_id");
    if (!id) return NULL;
    int idx = strada_to_int(id);
    if (idx < 0 || idx >= g_stmt_count) return NULL;
    return g_stmts[idx];
}

void strada_dbi_disconnect(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (dbh) dbi_disconnect(dbh);
}

StradaValue* strada_dbi_prepare(StradaValue *dbh_sv, StradaValue *sql) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_undef();

    const char *sql_str = sql ? strada_to_str(sql) : NULL;
    DbiStatement *sth = dbi_prepare(dbh, sql_str);
    if (!sth) return strada_new_undef();

    int idx = g_stmt_count++;
    g_stmts[idx] = sth;

    StradaValue *result = strada_new_hash();
    strada_hash_set(result->value.hv, "_stmt_id", strada_new_int(idx));
    strada_hash_set(result->value.hv, "NUM_OF_PARAMS", strada_new_int(sth->num_params));

    return result;
}

StradaValue* strada_dbi_execute(StradaValue *sth_sv, StradaValue *params) {
    DbiStatement *sth = get_sth(sth_sv);
    if (!sth) return strada_new_int(-1);

    int result = dbi_execute(sth, params);
    return strada_new_int(result);
}

StradaValue* strada_dbi_do(StradaValue *dbh_sv, StradaValue *sql, StradaValue *params) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(-1);

    const char *sql_str = sql ? strada_to_str(sql) : NULL;
    int result = dbi_do(dbh, sql_str, params);
    return strada_new_int(result);
}

StradaValue* strada_dbi_fetchrow_array(StradaValue *sth_sv) {
    DbiStatement *sth = get_sth(sth_sv);
    if (!sth) return strada_new_undef();
    return dbi_fetchrow_array(sth);
}

StradaValue* strada_dbi_fetchrow_hashref(StradaValue *sth_sv) {
    DbiStatement *sth = get_sth(sth_sv);
    if (!sth) return strada_new_undef();
    return dbi_fetchrow_hashref(sth);
}

StradaValue* strada_dbi_fetchall_arrayref(StradaValue *sth_sv) {
    DbiStatement *sth = get_sth(sth_sv);
    if (!sth) return strada_new_array();
    return dbi_fetchall_arrayref(sth);
}

StradaValue* strada_dbi_begin_work(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(-1);
    return strada_new_int(dbi_begin_work(dbh));
}

StradaValue* strada_dbi_commit(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(-1);
    return strada_new_int(dbi_commit(dbh));
}

StradaValue* strada_dbi_rollback(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(-1);
    return strada_new_int(dbi_rollback(dbh));
}

StradaValue* strada_dbi_quote(StradaValue *dbh_sv, StradaValue *str) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_undef();

    const char *s = str ? strada_to_str(str) : NULL;
    char *quoted = dbi_quote(dbh, s);
    StradaValue *result = strada_new_str(quoted);
    free(quoted);
    return result;
}

StradaValue* strada_dbi_last_insert_id(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(-1);
    return strada_new_int(dbi_last_insert_id(dbh, NULL, NULL, NULL, NULL));
}

StradaValue* strada_dbi_errstr(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_str("No database handle");
    return strada_new_str(dbi_errstr(dbh));
}

StradaValue* strada_dbi_err(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(-1);
    return strada_new_int(dbi_err(dbh));
}

void strada_dbi_finish(StradaValue *sth_sv) {
    DbiStatement *sth = get_sth(sth_sv);
    if (sth) dbi_finish(sth);
}

StradaValue* strada_dbi_rows(StradaValue *sth_sv) {
    DbiStatement *sth = get_sth(sth_sv);
    if (!sth) return strada_new_int(-1);
    return strada_new_int(dbi_rows(sth));
}

StradaValue* strada_dbi_column_names(StradaValue *sth_sv) {
    DbiStatement *sth = get_sth(sth_sv);
    if (!sth) return strada_new_array();
    return dbi_column_names(sth);
}

StradaValue* strada_dbi_ping(StradaValue *dbh_sv) {
    DbiHandle *dbh = get_dbh(dbh_sv);
    if (!dbh) return strada_new_int(0);
    return strada_new_int(dbi_ping(dbh));
}

/* ============== Raw C Functions for extern "C" ============== */

/* Connect using raw C types */
DbiHandle* strada_dbi_connect_raw(const char *dsn, const char *user, const char *pass,
                                   int auto_commit, int print_error) {
    DbiHandle *dbh = calloc(1, sizeof(DbiHandle));
    if (!dbh) return NULL;

    dbh->auto_commit = auto_commit;
    dbh->raise_error = 0;
    dbh->print_error = print_error;

    char *database = NULL;
    char *host = NULL;
    int port = 0;

    dbh->driver = parse_dsn(dsn, &database, &host, &port);
    dbh->dsn = dsn ? strdup(dsn) : NULL;
    dbh->username = user ? strdup(user) : NULL;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3 *db;
            int rc = sqlite3_open(database ? database : ":memory:", &db);
            if (rc != SQLITE_OK) {
                set_error(dbh, rc, sqlite3_errmsg(db));
                sqlite3_close(db);
                free(database);
                free(host);
                free(dbh->dsn);
                free(dbh->username);
                free(dbh);
                return NULL;
            }
            dbh->conn = db;
            dbh->connected = 1;
            break;
        }
#endif

#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL: {
            MYSQL *mysql = mysql_init(NULL);
            if (!mysql) {
                set_error(dbh, -1, "MySQL initialization failed");
                free(database);
                free(host);
                free(dbh->dsn);
                free(dbh->username);
                free(dbh);
                return NULL;
            }

            if (!mysql_real_connect(mysql,
                                   host ? host : "localhost",
                                   user,
                                   pass,
                                   database,
                                   port ? port : 3306,
                                   NULL, 0)) {
                set_error(dbh, mysql_errno(mysql), mysql_error(mysql));
                mysql_close(mysql);
                free(database);
                free(host);
                free(dbh->dsn);
                free(dbh->username);
                free(dbh);
                return NULL;
            }

            if (dbh->auto_commit) {
                mysql_autocommit(mysql, 1);
            }

            dbh->conn = mysql;
            dbh->connected = 1;
            break;
        }
#endif

#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            char conninfo[1024];
            snprintf(conninfo, sizeof(conninfo),
                    "host=%s port=%d dbname=%s user=%s password=%s",
                    host ? host : "localhost",
                    port ? port : 5432,
                    database ? database : "",
                    user ? user : "",
                    pass ? pass : "");

            PGconn *pg = PQconnectdb(conninfo);
            if (PQstatus(pg) != CONNECTION_OK) {
                set_error(dbh, -1, PQerrorMessage(pg));
                PQfinish(pg);
                free(database);
                free(host);
                free(dbh->dsn);
                free(dbh->username);
                free(dbh);
                return NULL;
            }

            dbh->conn = pg;
            dbh->connected = 1;
            break;
        }
#endif

        default:
            set_error(dbh, -1, "Unknown or unsupported database driver");
            free(database);
            free(host);
            free(dbh->dsn);
            free(dbh->username);
            free(dbh);
            return NULL;
    }

    free(database);
    free(host);
    return dbh;
}

/* Disconnect using raw ptr */
void strada_dbi_disconnect_raw(DbiHandle *dbh) {
    dbi_disconnect(dbh);
}

/* Ping using raw ptr */
int strada_dbi_ping_raw(DbiHandle *dbh) {
    return dbi_ping(dbh);
}

/* Prepare statement using raw types */
DbiStatement* strada_dbi_prepare_raw(DbiHandle *dbh, const char *sql) {
    return dbi_prepare(dbh, sql);
}

/* Execute without params (params bound separately) */
int strada_dbi_execute_raw(DbiStatement *sth) {
    if (!sth || !sth->dbh) return -1;

    DbiHandle *dbh = sth->dbh;
    sth->executed = 1;
    sth->finished = 0;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            sqlite3_stmt *stmt = (sqlite3_stmt*)sth->stmt;
            int rc = sqlite3_step(stmt);

            if (rc == SQLITE_DONE) {
                sth->affected_rows = sqlite3_changes((sqlite3*)dbh->conn);
                sth->finished = 1;
                return sth->affected_rows;
            } else if (rc == SQLITE_ROW) {
                /* SELECT query - reset to allow step-based fetching */
                sqlite3_reset(stmt);
                return 0;
            } else {
                set_error(dbh, rc, sqlite3_errmsg((sqlite3*)dbh->conn));
                return -1;
            }
        }
#endif
#ifdef HAVE_MYSQL
        case DBI_DRIVER_MYSQL: {
            MYSQL_STMT *stmt = (MYSQL_STMT*)sth->stmt;
            if (mysql_stmt_execute(stmt) != 0) {
                set_error(dbh, mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
                return -1;
            }
            sth->affected_rows = mysql_stmt_affected_rows(stmt);
            return sth->affected_rows;
        }
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            /* For PostgreSQL, we store the SQL and execute on step */
            return 0;
        }
#endif
        default:
            return -1;
    }
}

/* Bind int parameter */
void strada_dbi_bind_int(DbiStatement *sth, int idx, int64_t val) {
    if (!sth || !sth->dbh) return;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            sqlite3_bind_int64((sqlite3_stmt*)sth->stmt, idx, val);
            break;
#endif
        default:
            break;
    }
}

/* Bind string parameter */
void strada_dbi_bind_str(DbiStatement *sth, int idx, const char *val) {
    if (!sth || !sth->dbh) return;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            if (val) {
                sqlite3_bind_text((sqlite3_stmt*)sth->stmt, idx, val, -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null((sqlite3_stmt*)sth->stmt, idx);
            }
            break;
#endif
        default:
            break;
    }
}

/* Bind double parameter */
void strada_dbi_bind_num(DbiStatement *sth, int idx, double val) {
    if (!sth || !sth->dbh) return;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            sqlite3_bind_double((sqlite3_stmt*)sth->stmt, idx, val);
            break;
#endif
        default:
            break;
    }
}

/* Bind NULL parameter */
void strada_dbi_bind_null(DbiStatement *sth, int idx) {
    if (!sth || !sth->dbh) return;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            sqlite3_bind_null((sqlite3_stmt*)sth->stmt, idx);
            break;
#endif
        default:
            break;
    }
}

/* Clear bindings */
void strada_dbi_clear_bindings(DbiStatement *sth) {
    if (!sth || !sth->dbh) return;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            sqlite3_reset((sqlite3_stmt*)sth->stmt);
            sqlite3_clear_bindings((sqlite3_stmt*)sth->stmt);
            break;
#endif
        default:
            break;
    }
}

/* Step to next row - returns 1 if row available, 0 if done, -1 on error */
int strada_dbi_step(DbiStatement *sth) {
    if (!sth || !sth->dbh || !sth->executed) return -1;
    if (sth->finished) return 0;

    DbiHandle *dbh = sth->dbh;

    switch (dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            int rc = sqlite3_step((sqlite3_stmt*)sth->stmt);
            if (rc == SQLITE_ROW) {
                return 1;
            } else if (rc == SQLITE_DONE) {
                sth->finished = 1;
                return 0;
            } else {
                set_error(dbh, rc, sqlite3_errmsg((sqlite3*)dbh->conn));
                return -1;
            }
        }
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            /* PostgreSQL: increment row counter */
            if (!sth->result) return 0;
            PGresult *res = (PGresult*)sth->result;
            if (sth->row_count < PQntuples(res)) {
                return 1;
            }
            sth->finished = 1;
            return 0;
        }
#endif
        default:
            return -1;
    }
}

/* Get column count */
int strada_dbi_column_count(DbiStatement *sth) {
    if (!sth) return 0;
    return sth->num_columns;
}

/* Get column name - returns allocated string, caller must free */
char* strada_dbi_column_name(DbiStatement *sth, int idx) {
    if (!sth || !sth->column_names || idx < 0 || idx >= sth->num_columns) {
        return strdup("");
    }
    return strdup(sth->column_names[idx]);
}

/* Get column type: 0=null, 1=int, 2=num, 3=str */
int strada_dbi_column_type(DbiStatement *sth, int idx) {
    if (!sth || !sth->dbh) return 0;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            int type = sqlite3_column_type((sqlite3_stmt*)sth->stmt, idx);
            switch (type) {
                case SQLITE_NULL:    return 0;
                case SQLITE_INTEGER: return 1;
                case SQLITE_FLOAT:   return 2;
                default:             return 3;
            }
        }
#endif
        default:
            return 3; /* Default to string */
    }
}

/* Get column as int */
int64_t strada_dbi_column_int(DbiStatement *sth, int idx) {
    if (!sth || !sth->dbh) return 0;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            return sqlite3_column_int64((sqlite3_stmt*)sth->stmt, idx);
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            if (!sth->result) return 0;
            PGresult *res = (PGresult*)sth->result;
            /* Use a row tracker stored in affected_rows temporarily */
            int row = sth->affected_rows;
            if (PQgetisnull(res, row, idx)) return 0;
            return strtoll(PQgetvalue(res, row, idx), NULL, 10);
        }
#endif
        default:
            return 0;
    }
}

/* Get column as double */
double strada_dbi_column_num(DbiStatement *sth, int idx) {
    if (!sth || !sth->dbh) return 0.0;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            return sqlite3_column_double((sqlite3_stmt*)sth->stmt, idx);
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            if (!sth->result) return 0.0;
            PGresult *res = (PGresult*)sth->result;
            int row = sth->affected_rows;
            if (PQgetisnull(res, row, idx)) return 0.0;
            return strtod(PQgetvalue(res, row, idx), NULL);
        }
#endif
        default:
            return 0.0;
    }
}

/* Get column as string - returns allocated string, caller must free */
char* strada_dbi_column_str(DbiStatement *sth, int idx) {
    if (!sth || !sth->dbh) return strdup("");

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE: {
            const char *text = (const char*)sqlite3_column_text((sqlite3_stmt*)sth->stmt, idx);
            return strdup(text ? text : "");
        }
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            if (!sth->result) return strdup("");
            PGresult *res = (PGresult*)sth->result;
            int row = sth->affected_rows;
            if (PQgetisnull(res, row, idx)) return strdup("");
            return strdup(PQgetvalue(res, row, idx));
        }
#endif
        default:
            return strdup("");
    }
}

/* Check if column is null */
int strada_dbi_column_is_null(DbiStatement *sth, int idx) {
    if (!sth || !sth->dbh) return 1;

    switch (sth->dbh->driver) {
#ifdef HAVE_SQLITE3
        case DBI_DRIVER_SQLITE:
            return sqlite3_column_type((sqlite3_stmt*)sth->stmt, idx) == SQLITE_NULL;
#endif
#ifdef HAVE_POSTGRES
        case DBI_DRIVER_POSTGRES: {
            if (!sth->result) return 1;
            PGresult *res = (PGresult*)sth->result;
            int row = sth->affected_rows;
            return PQgetisnull(res, row, idx);
        }
#endif
        default:
            return 1;
    }
}

/* Do SQL directly (no params) */
int strada_dbi_do_raw(DbiHandle *dbh, const char *sql) {
    if (!dbh || !sql) return -1;
    return dbi_do(dbh, sql, NULL);
}

/* Transaction functions */
int strada_dbi_begin_work_raw(DbiHandle *dbh) {
    return dbi_begin_work(dbh);
}

int strada_dbi_commit_raw(DbiHandle *dbh) {
    return dbi_commit(dbh);
}

int strada_dbi_rollback_raw(DbiHandle *dbh) {
    return dbi_rollback(dbh);
}

/* Quote string - returns allocated string, caller must free */
char* strada_dbi_quote_raw(DbiHandle *dbh, const char *str) {
    return dbi_quote(dbh, str);
}

/* Get last insert ID */
int64_t strada_dbi_last_insert_id_raw(DbiHandle *dbh) {
    return dbi_last_insert_id(dbh, NULL, NULL, NULL, NULL);
}

/* Get error string - returns static string, do not free */
const char* strada_dbi_errstr_raw(DbiHandle *dbh) {
    return dbi_errstr(dbh);
}

/* Get error code */
int strada_dbi_err_raw(DbiHandle *dbh) {
    return dbi_err(dbh);
}

/* Get affected rows */
int strada_dbi_rows_raw(DbiStatement *sth) {
    return dbi_rows(sth);
}

/* Finish statement */
void strada_dbi_finish_raw(DbiStatement *sth) {
    dbi_finish(sth);
}

/* Free statement */
void strada_dbi_free_statement_raw(DbiStatement *sth) {
    dbi_free_statement(sth);
}

/* Get number of params */
int strada_dbi_num_params(DbiStatement *sth) {
    if (!sth) return 0;
    return sth->num_params;
}
