// odbc_cli.c
//
// A small, timing-aware ODBC command-line client for the madras ODBC
// driver -- isql/iusql don't display elapsed time at all, which was the
// direct motivation for this. Mirrors com.madras.sql.tools.SqlCli's
// feature set (compare/prepare/exec meta-commands) but talks real ODBC
// directly, and works against ANY ODBC driver/DSN, not just this one.
//
// Uses GNU readline for line editing/history (unlike the JDBC CLI's
// JLine dependency, this was verified working end-to-end in the same
// environment that built it -- readline is a small, near-universally
// available C library, not a Maven-only Java dependency).

// _POSIX_C_SOURCE is needed on Linux/glibc to expose clock_gettime()
// under strict -std=c11 (confirmed directly: removing it broke the Linux
// build) -- but defining it on macOS/Apple's libc without also defining
// _DARWIN_C_SOURCE restricts header exposure in ways that broke snprintf
// there instead (also confirmed directly, on real macOS/M1 hardware).
// Apple's libc has provided clock_gettime()/CLOCK_MONOTONIC natively
// (via <time.h>, no feature-test macros needed) since macOS 10.12, well
// before any M1 Mac shipped, so this define is only needed -- and only
// safe -- on non-Apple platforms.
#ifndef __APPLE__
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdint.h>

#include <sql.h>
#include <sqlext.h>

#include <readline/readline.h>
#include <readline/history.h>

static SQLHENV g_env;
static SQLHDBC g_dbc;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void print_diag(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR sqlstate[6], msg[512];
    SQLINTEGER native;
    SQLSMALLINT len;
    SQLRETURN r = SQLGetDiagRec(handle_type, handle, 1, sqlstate, &native, msg, sizeof(msg), &len);
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        printf("ERROR [%s]: %s\n", sqlstate, msg);
    } else {
        printf("ERROR (no diagnostic record available)\n");
    }
}

// Runs one query via SQLExecDirect, timing execute and fetch separately
// (matching the JDBC SqlCli's own executeQuery/fetch split), printing a
// preview of up to 10 rows plus row count and elapsed time.
static void run_query(const char *sql) {
    SQLHSTMT stmt;
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &stmt);

    double t0 = now_ms();
    SQLRETURN r = SQLExecDirect(stmt, (SQLCHAR *) sql, SQL_NTS);
    double exec_ms = now_ms() - t0;

    if (r != SQL_SUCCESS && r != SQL_SUCCESS_WITH_INFO) {
        print_diag(SQL_HANDLE_STMT, stmt);
        printf("(%.1f ms)\n\n", exec_ms);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return;
    }

    SQLSMALLINT col_count = 0;
    SQLNumResultCols(stmt, &col_count);

    double t1 = now_ms();
    long row_count = 0;
    const int preview_limit = 10;
    char buf[1024];
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        if (row_count < preview_limit) {
            printf("  ");
            for (SQLSMALLINT c = 1; c <= col_count; c++) {
                SQLLEN ind = 0;
                SQLGetData(stmt, c, SQL_C_CHAR, buf, sizeof(buf), &ind);
                if (c > 1) printf("\t");
                printf("%s", ind == SQL_NULL_DATA ? "(null)" : buf);
            }
            printf("\n");
        } else if (row_count == preview_limit) {
            printf("  ... (more rows follow, not printed)\n");
        }
        row_count++;
    }
    double fetch_ms = now_ms() - t1;
    double total_ms = now_ms() - t0;

    printf("rows=%ld  executeQuery=%.1fms  fetch=%.1fms  total=%.1fms\n\n",
           row_count, exec_ms, fetch_ms, total_ms);

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

// Compares Statement-style (fresh literal substitution + SQLExecDirect
// each call, always re-parses/re-plans) against PreparedStatement-style
// (SQLPrepare once, SQLBindParameter+SQLExecute per call) over n
// iterations, cycling a bound value base..base+9 -- same methodology as
// SqlCli.java's .compare, for apples-to-apples comparison.
static void run_compare(const char *sql_template, int n, long base) {
    // Suppresses "Using index: ..." logging for the duration of the timed
    // loops -- MADRAS_ATTR_INDEX_LOGGING is a driver-specific connection
    // attribute (see madras_odbc_driver.cpp), the standard ODBC mechanism
    // for this kind of driver-specific toggle. Without this, thousands
    // of log lines both flood the output and add real stderr I/O
    // overhead that would skew the very timing being measured.
    SQLSetConnectAttr(g_dbc, 12001, (SQLPOINTER) (intptr_t) 0, 0);

    SQLHSTMT stmt1;
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &stmt1);
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        char literal_sql[2048];
        char valbuf[32];
        snprintf(valbuf, sizeof(valbuf), "%ld", base + (i % 10));
        const char *qmark = strchr(sql_template, '?');
        if (!qmark) { printf("no '?' placeholder found in template\n"); SQLFreeHandle(SQL_HANDLE_STMT, stmt1); return; }
        size_t prefix_len = qmark - sql_template;
        snprintf(literal_sql, sizeof(literal_sql), "%.*s%s%s", (int) prefix_len, sql_template, valbuf, qmark + 1);

        SQLExecDirect(stmt1, (SQLCHAR *) literal_sql, SQL_NTS);
        while (SQLFetch(stmt1) == SQL_SUCCESS) { /* drain */ }
        SQLFreeStmt(stmt1, SQL_CLOSE);
    }
    double literal_ms = now_ms() - t0;
    SQLFreeHandle(SQL_HANDLE_STMT, stmt1);

    SQLHSTMT stmt2;
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &stmt2);
    SQLRETURN pr = SQLPrepare(stmt2, (SQLCHAR *) sql_template, SQL_NTS);
    if (pr != SQL_SUCCESS && pr != SQL_SUCCESS_WITH_INFO) {
        print_diag(SQL_HANDLE_STMT, stmt2);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt2);
        return;
    }
    SQLLEN bound_val;
    SQLBindParameter(stmt2, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &bound_val, 0, NULL);
    t0 = now_ms();
    for (int i = 0; i < n; i++) {
        bound_val = base + (i % 10);
        SQLExecute(stmt2);
        while (SQLFetch(stmt2) == SQL_SUCCESS) { /* drain */ }
        SQLFreeStmt(stmt2, SQL_CLOSE);
    }
    double prepared_ms = now_ms() - t0;
    SQLFreeHandle(SQL_HANDLE_STMT, stmt2);

    printf("Statement (re-parse each call):    %8.1f ms  (%.4f ms/call)\n",
           literal_ms, literal_ms / n);
    printf("PreparedStatement (prepared once): %8.1f ms  (%.4f ms/call)\n",
           prepared_ms, prepared_ms / n);
    printf("speedup: %.2fx\n\n", literal_ms / prepared_ms);

    SQLSetConnectAttr(g_dbc, 12001, (SQLPOINTER) (intptr_t) 1, 0);
}

// State for .prepare / .exec
static SQLHSTMT g_prepared = SQL_NULL_HSTMT;
static SQLLEN g_prepared_params[16];
static int g_prepared_param_count = 0;

static void run_prepare(const char *sql) {
    if (g_prepared != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, g_prepared);
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &g_prepared);
    double t0 = now_ms();
    SQLRETURN r = SQLPrepare(g_prepared, (SQLCHAR *) sql, SQL_NTS);
    double ms = now_ms() - t0;
    if (r != SQL_SUCCESS && r != SQL_SUCCESS_WITH_INFO) {
        print_diag(SQL_HANDLE_STMT, g_prepared);
        SQLFreeHandle(SQL_HANDLE_STMT, g_prepared);
        g_prepared = SQL_NULL_HSTMT;
        return;
    }
    g_prepared_param_count = 0;
    for (const char *p = sql; *p; p++) if (*p == '?') g_prepared_param_count++;
    if (g_prepared_param_count > 16) g_prepared_param_count = 16;
    for (int i = 0; i < g_prepared_param_count; i++) {
        SQLBindParameter(g_prepared, i + 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                          &g_prepared_params[i], 0, NULL);
    }
    printf("prepared (%.2f ms), %d parameter(s) -- use .exec <values...> to run it\n\n", ms, g_prepared_param_count);
}

static void run_exec(char *args) {
    if (g_prepared == SQL_NULL_HSTMT) {
        printf("no statement prepared -- use .prepare <sql with ?> first\n\n");
        return;
    }
    int i = 0;
    char *tok = strtok(args, " \t");
    while (tok && i < g_prepared_param_count) {
        g_prepared_params[i] = atol(tok);
        tok = strtok(NULL, " \t");
        i++;
    }
    double t0 = now_ms();
    SQLRETURN r = SQLExecute(g_prepared);
    double exec_ms = now_ms() - t0;
    if (r != SQL_SUCCESS && r != SQL_SUCCESS_WITH_INFO) {
        print_diag(SQL_HANDLE_STMT, g_prepared);
        SQLFreeStmt(g_prepared, SQL_CLOSE);
        printf("(%.1f ms)\n\n", exec_ms);
        return;
    }
    SQLSMALLINT col_count = 0;
    SQLNumResultCols(g_prepared, &col_count);
    long row_count = 0;
    char buf[1024];
    const int preview_limit = 10;
    while (SQLFetch(g_prepared) == SQL_SUCCESS) {
        if (row_count < preview_limit) {
            printf("  ");
            for (SQLSMALLINT c = 1; c <= col_count; c++) {
                SQLLEN ind = 0;
                SQLGetData(g_prepared, c, SQL_C_CHAR, buf, sizeof(buf), &ind);
                if (c > 1) printf("\t");
                printf("%s", ind == SQL_NULL_DATA ? "(null)" : buf);
            }
            printf("\n");
        }
        row_count++;
    }
    double total_ms = now_ms() - t0;
    printf("rows=%ld  executeQuery=%.1fms  total=%.1fms\n\n", row_count, exec_ms, total_ms);
    SQLFreeStmt(g_prepared, SQL_CLOSE);
}

static void print_help(void) {
    printf("  .compare <n> <base> <sql with one ?>\n");
    printf("               compare Statement (re-parse each call) vs\n");
    printf("               PreparedStatement (prepared once) over n calls,\n");
    printf("               binding base..base+9 cyclically\n");
    printf("  .prepare <sql with ?>   prepare a statement, held for .exec\n");
    printf("  .exec <v1> <v2> ...     bind (as SQLBIGINT) and run the prepared statement\n");
    printf("  .dt                     list tables\n");
    printf("  .d <table>              describe a table's columns\n");
    printf("  .log on|off             toggle \"Using index: ...\" diagnostic logging (default on)\n");
    printf("  exit / quit             disconnect and exit\n\n");
}

static void run_catalog(int is_columns, const char *table) {
    SQLHSTMT stmt;
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &stmt);
    if (is_columns) {
        SQLColumns(stmt, NULL, 0, NULL, 0, (SQLCHAR *) table, SQL_NTS, NULL, 0);
    } else {
        SQLTables(stmt, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    }
    SQLSMALLINT col_count = 0;
    SQLNumResultCols(stmt, &col_count);
    char buf[256];
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        printf("  ");
        for (SQLSMALLINT c = 1; c <= col_count; c++) {
            SQLLEN ind = 0;
            SQLGetData(stmt, c, SQL_C_CHAR, buf, sizeof(buf), &ind);
            if (c > 1) printf("\t");
            printf("%s", ind == SQL_NULL_DATA ? "" : buf);
        }
        printf("\n");
    }
    printf("\n");
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <connection-string> [\"<sql>\"]\n", argv[0]);
        fprintf(stderr, "  e.g.: %s \"DRIVER={Madras};DBQ=/path/to/file.mdsi;\"\n", argv[0]);
        return 1;
    }

    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_env);
    SQLSetEnvAttr(g_env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER) SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, g_env, &g_dbc);

    printf("Connecting: %s\n", argv[1]);
    double t0 = now_ms();
    SQLCHAR out_conn[1024];
    SQLSMALLINT out_len;
    SQLRETURN r = SQLDriverConnect(g_dbc, NULL, (SQLCHAR *) argv[1], SQL_NTS,
                                    out_conn, sizeof(out_conn), &out_len, SQL_DRIVER_NOPROMPT);
    if (r != SQL_SUCCESS && r != SQL_SUCCESS_WITH_INFO) {
        print_diag(SQL_HANDLE_DBC, g_dbc);
        return 1;
    }
    printf("Connected in %.1f ms\n\n", now_ms() - t0);

    if (argc >= 3) {
        run_query(argv[2]);
        SQLDisconnect(g_dbc);
        return 0;
    }

    printf("Interactive mode -- 'exit' to quit, '.?' for meta-commands:\n");
    using_history();
    char histfile[512];
    snprintf(histfile, sizeof(histfile), "%s/.madras_odbc_cli_history", getenv("HOME") ? getenv("HOME") : ".");
    read_history(histfile);

    char *line;
    while ((line = readline("sql> ")) != NULL) {
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t' || trimmed[len - 1] == '\n')) {
            trimmed[--len] = '\0';
        }
        if (len == 0) { free(line); continue; }
        add_history(trimmed);
        write_history(histfile);

        if (strcasecmp(trimmed, "exit") == 0 || strcasecmp(trimmed, "quit") == 0) {
            free(line);
            break;
        }
        if (len > 0 && trimmed[len - 1] == ';') trimmed[--len] = '\0';

        if (trimmed[0] == '.') {
            if (strcmp(trimmed, ".?") == 0) {
                print_help();
            } else if (strncmp(trimmed, ".compare ", 9) == 0) {
                int n; long base; char sql[2048];
                if (sscanf(trimmed + 9, "%d %ld %[^\n]", &n, &base, sql) == 3) {
                    run_compare(sql, n, base);
                } else {
                    printf("usage: .compare <n> <base> <sql with one ?>\n\n");
                }
            } else if (strncmp(trimmed, ".prepare ", 9) == 0) {
                run_prepare(trimmed + 9);
            } else if (strncmp(trimmed, ".exec", 5) == 0) {
                run_exec(trimmed + 5);
            } else if (strcmp(trimmed, ".dt") == 0) {
                run_catalog(0, NULL);
            } else if (strncmp(trimmed, ".log", 4) == 0) {
                char *rest = trimmed + 4;
                while (*rest == ' ') rest++;
                if (strcmp(rest, "on") == 0) {
                    SQLSetConnectAttr(g_dbc, 12001, (SQLPOINTER) (intptr_t) 1, 0);
                    printf("index logging: on\n\n");
                } else if (strcmp(rest, "off") == 0) {
                    SQLSetConnectAttr(g_dbc, 12001, (SQLPOINTER) (intptr_t) 0, 0);
                    printf("index logging: off\n\n");
                } else {
                    printf("usage: .log on|off\n\n");
                }
            } else if (strncmp(trimmed, ".d", 2) == 0) {
                const char *table = (len > 2) ? trimmed + 3 : "t";
                run_catalog(1, table);
            } else {
                printf("Unknown meta-command: %s (try .?)\n\n", trimmed);
            }
        } else {
            run_query(trimmed);
        }
        free(line);
    }

    if (g_prepared != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, g_prepared);
    SQLDisconnect(g_dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, g_dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, g_env);
    printf("Disconnected.\n");
    return 0;
}
