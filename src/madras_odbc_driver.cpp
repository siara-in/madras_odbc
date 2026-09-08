// madras_odbc_driver.cpp
//
// A minimal but genuinely usable ODBC driver around dv1sql::engine --
// wraps the SAME C++ SQL engine used by the JNI/JDBC layer, as a
// separate protocol adapter (no new SQL logic here, purely a C-ABI
// wrapper). Scoped deliberately narrow: enough for real tools (isql,
// pyodbc, Excel/BI generic-ODBC connectors) to connect and run SELECT
// queries, not a fully spec-conformant driver (no DML, no transactions,
// no Unicode/SQLWCHAR entry points, no scrollable cursors).
//
// Values are exposed as SQL_CHAR (text) throughout -- matches the
// engine's own already-text-formatted query_result, and is a reasonable,
// common simplification for a v1 driver. SQLGetData/bound-column fetch
// still do on-demand conversion to whatever C type the caller actually
// asked for (SQL_C_LONG, SQL_C_DOUBLE, etc.), so typed access still works
// correctly -- only the *wire* representation is uniformly text.

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <odbcinst.h>

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#include "madras/dv1/reader/static_trie_map.hpp"
#include "dv1/engine.hpp"

using namespace madras::dv1;

// ---------------------------------------------------------------------
// Handle structures
// ---------------------------------------------------------------------

struct OdbcEnv {
    SQLINTEGER odbc_version = SQL_OV_ODBC3;
};

struct OdbcDiag {
    std::string sqlstate = "00000";
    std::string message;
    bool has_error = false;
};

struct OdbcConn {
    std::unique_ptr<static_trie_map> stm;
    std::unique_ptr<dv1sql::engine> eng;
    OdbcDiag diag;
    bool connected = false;
};

// A bound parameter (SQLBindParameter) -- the actual value isn't known
// until SQLExecute is called (the app fills app_buffer after binding),
// so we store the buffer description and resolve to a string at execute
// time, not at bind time.
struct BoundParam {
    SQLSMALLINT c_type = SQL_C_CHAR;
    SQLPOINTER app_buffer = nullptr;
    SQLLEN buffer_len = 0;
    SQLLEN *str_len_or_ind = nullptr;
};

// A bound column (SQLBindCol) -- filled automatically on each SQLFetch,
// unlike SQLGetData which is pulled on demand.
struct BoundCol {
    bool bound = false;
    SQLSMALLINT c_type = SQL_C_CHAR;
    SQLPOINTER app_buffer = nullptr;
    SQLLEN buffer_len = 0;
    SQLLEN *str_len_or_ind = nullptr;
};

struct OdbcStmt {
    OdbcConn *conn = nullptr;
    OdbcDiag diag;

    std::string sql_text;             // set by SQLPrepare or SQLExecDirect
    sql::query_plan *prepared_plan = nullptr; // non-null after SQLPrepare
    int param_count = 0;
    std::vector<BoundParam> bound_params;

    dv1sql::query_result result;
    bool has_result = false;
    long long current_row = -1;       // -1 = before first row (matches SQLFetch semantics)

    std::vector<BoundCol> bound_cols;
};

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void SetError(OdbcDiag &diag, const std::string &sqlstate, const std::string &msg) {
    diag.sqlstate = sqlstate;
    diag.message = msg;
    diag.has_error = true;
}

static void ClearError(OdbcDiag &diag) {
    diag.has_error = false;
    diag.message.clear();
    diag.sqlstate = "00000";
}

// Parses a DSN-less connection string like "DBQ=/path/to/file.mdsi;" (also
// accepts a bare path with no "DBQ=" for convenience, and is
// case-insensitive on the keyword) into the .mdsi file path.
//
// Also resolves "DSN=<name>;" by looking up that DSN's own DBQ= entry in
// odbc.ini -- confirmed as necessary, not a hypothetical: iusql (the
// Unicode isql variant) calls SQLDriverConnect(W) directly with a
// "DSN=MadrasTest;"-style string (not SQLConnect(W), which already did
// its own DSN resolution) -- without this, DoConnect literally tried to
// open a file named "DSN=MadrasTest" and failed with ENOENT.
static std::string ExtractDbq(const std::string &conn_str) {
    std::string upper = conn_str;
    for (auto &c : upper) c = (char) toupper((unsigned char) c);

    size_t dbq_pos = upper.find("DBQ=");
    if (dbq_pos != std::string::npos) {
        size_t start = dbq_pos + 4;
        size_t end = conn_str.find(';', start);
        if (end == std::string::npos) end = conn_str.size();
        return conn_str.substr(start, end - start);
    }

    size_t dsn_pos = upper.find("DSN=");
    if (dsn_pos != std::string::npos) {
        size_t start = dsn_pos + 4;
        size_t end = conn_str.find(';', start);
        if (end == std::string::npos) end = conn_str.size();
        std::string dsn_name = conn_str.substr(start, end - start);
        char dbq[1024] = {0};
        SQLGetPrivateProfileString(dsn_name.c_str(), "DBQ", "", dbq, sizeof(dbq), "odbc.ini");
        return std::string(dbq);
    }

    // Neither keyword found -- treat the whole string as a bare path,
    // trimming any trailing ';' a caller might have added out of habit.
    std::string trimmed = conn_str;
    size_t semi = trimmed.find(';');
    if (semi != std::string::npos) trimmed = trimmed.substr(0, semi);
    return trimmed;
}

// Copies a text value into an ODBC caller-supplied buffer, honoring
// truncation and NULL-indication semantics: if the buffer is smaller than
// the (converted) value, copies as much as fits, null-terminates within
// the given length, sets str_len_or_ind to the FULL untruncated length,
// and returns SQL_SUCCESS_WITH_INFO (truncation is a warning, not a hard
// error, per spec) rather than silently under- or over-filling the
// buffer or over/under-reporting the indicator.
// Forward declaration -- Utf8ToUtf16Buffer is defined further down (after
// the UTF-16/UTF-8 conversion helpers), but CopyToBuffer needs it for its
// SQL_C_WCHAR case.
static SQLRETURN Utf8ToUtf16Buffer(const std::string &str, SQLWCHAR *buffer,
                                    SQLSMALLINT buf_len_chars, SQLSMALLINT *out_len_chars);

static SQLRETURN CopyToBuffer(const std::string *value, bool is_null,
                               SQLSMALLINT c_type, SQLPOINTER buffer, SQLLEN buffer_len,
                               SQLLEN *str_len_or_ind) {
    if (is_null || value == nullptr) {
        if (str_len_or_ind) *str_len_or_ind = SQL_NULL_DATA;
        return SQL_SUCCESS;
    }
    switch (c_type) {
        case SQL_C_LONG:
        case SQL_C_SLONG:
            if (buffer) *(SQLINTEGER *) buffer = (SQLINTEGER) atoll(value->c_str());
            if (str_len_or_ind) *str_len_or_ind = sizeof(SQLINTEGER);
            return SQL_SUCCESS;
        case SQL_C_SBIGINT:
            if (buffer) *(SQLBIGINT *) buffer = (SQLBIGINT) atoll(value->c_str());
            if (str_len_or_ind) *str_len_or_ind = sizeof(SQLBIGINT);
            return SQL_SUCCESS;
        case SQL_C_DOUBLE:
            if (buffer) *(SQLDOUBLE *) buffer = atof(value->c_str());
            if (str_len_or_ind) *str_len_or_ind = sizeof(SQLDOUBLE);
            return SQL_SUCCESS;
        case SQL_C_FLOAT:
            if (buffer) *(SQLREAL *) buffer = (SQLREAL) atof(value->c_str());
            if (str_len_or_ind) *str_len_or_ind = sizeof(SQLREAL);
            return SQL_SUCCESS;
        case SQL_C_WCHAR: {
            // Confirmed as a real, necessary case, not defensive
            // programming: a Unicode client (iusql) binding columns via
            // SQLBindCol requests SQL_C_WCHAR, and without this case it
            // silently fell through to the SQL_C_CHAR narrow-byte-copy
            // branch below -- writing narrow bytes into a buffer the
            // client interprets as UTF-16, corrupting every fetched
            // value (reproduced directly: "500000" became "500" -- a
            // length in BYTES being read back as a length in
            // CHARACTERS, exactly the narrow/wide confusion this
            // case fixes).
            SQLSMALLINT out_len_chars = 0;
            SQLRETURN r = Utf8ToUtf16Buffer(*value, (SQLWCHAR *) buffer,
                                             (SQLSMALLINT) (buffer_len / (SQLLEN) sizeof(SQLWCHAR)), &out_len_chars);
            if (str_len_or_ind) *str_len_or_ind = (SQLLEN) out_len_chars * (SQLLEN) sizeof(SQLWCHAR);
            return r;
        }
        case SQL_C_CHAR:
        default: {
            size_t full_len = value->size();
            if (str_len_or_ind) *str_len_or_ind = (SQLLEN) full_len;
            if (buffer == nullptr || buffer_len <= 0) return SQL_SUCCESS;
            size_t copy_len = full_len;
            bool truncated = false;
            if (copy_len >= (size_t) buffer_len) {
                copy_len = (size_t) buffer_len - 1;
                truncated = true;
            }
            memcpy(buffer, value->c_str(), copy_len);
            ((char *) buffer)[copy_len] = '\0';
            return truncated ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
        }
    }
}

// Reads a NULL-terminated bound parameter's app buffer and converts it
// to the text representation the engine's apply_param_patches() expects.
static std::string ParamToString(const BoundParam &p) {
    if (p.str_len_or_ind != nullptr && *p.str_len_or_ind == SQL_NULL_DATA) return "";
    switch (p.c_type) {
        case SQL_C_LONG:
        case SQL_C_SLONG:
            return std::to_string(*(SQLINTEGER *) p.app_buffer);
        case SQL_C_SBIGINT:
            return std::to_string(*(SQLBIGINT *) p.app_buffer);
        case SQL_C_DOUBLE:
            return std::to_string(*(SQLDOUBLE *) p.app_buffer);
        case SQL_C_FLOAT:
            return std::to_string(*(SQLREAL *) p.app_buffer);
        case SQL_C_CHAR:
        default: {
            SQLLEN len = p.str_len_or_ind ? *p.str_len_or_ind : SQL_NTS;
            if (len == SQL_NTS) return std::string((const char *) p.app_buffer);
            return std::string((const char *) p.app_buffer, (size_t) len);
        }
    }
}

// ---------------------------------------------------------------------
// Unicode (SQLWCHAR) <-> std::string conversion.
//
// SQLWCHAR is 2 bytes here (confirmed via this build's sqltypes.h:
// SQL_WCHART_CONVERT is not defined, so WCHAR = unsigned short) --
// matches the near-universal ODBC default on both Linux/unixODBC and
// Windows. Handles the Basic Multilingual Plane correctly (everything
// ASCII/Latin/CJK/etc. without surrogate pairs) -- astral-plane
// characters (surrogate pairs, code points beyond U+FFFF, e.g. some
// emoji) are not specially handled and would produce incorrect output.
// Deliberately out of scope for this driver: SQL identifiers, file
// paths, and this engine's own text values are overwhelmingly within the
// BMP in practice.
//
// This gap (no Unicode entry points at all) was confirmed as a REAL,
// serious bug, not a hypothetical: a Unicode ODBC client (iusql)
// segfaulted against this driver on a small test file, and produced
// silently wrong results (a small garbage row count instead of the real
// 12.66M, garbage column values) against a large real file -- undefined
// behavior from the driver manager's fallback handling when a driver
// lacks the Unicode entry points a Unicode client calls, not a clean
// error. Reproduced directly before writing this fix, not assumed.
static std::string Utf16ToUtf8(const SQLWCHAR *wstr, SQLINTEGER len) {
    if (wstr == nullptr) return "";
    size_t n;
    if (len == SQL_NTS) {
        n = 0;
        while (wstr[n] != 0) n++;
    } else {
        n = (size_t) len;
    }
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        unsigned int cp = wstr[i];
        // Surrogate pairs: combine but this is the one BMP-only
        // simplification noted above -- we advance past the low
        // surrogate without computing the correct astral code point.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) { i++; cp = 0xFFFD; }
        if (cp < 0x80) {
            out.push_back((char) cp);
        } else if (cp < 0x800) {
            out.push_back((char) (0xC0 | (cp >> 6)));
            out.push_back((char) (0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char) (0xE0 | (cp >> 12)));
            out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char) (0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Writes `str` as UTF-16 into a caller-supplied SQLWCHAR buffer, honoring
// ODBC's truncation/length-reporting convention for W functions (lengths
// are in CHARACTERS, not bytes). Returns SQL_SUCCESS_WITH_INFO if
// truncated, matching CopyToBuffer's own convention for the ANSI path.
static SQLRETURN Utf8ToUtf16Buffer(const std::string &str, SQLWCHAR *buffer,
                                    SQLSMALLINT buf_len_chars, SQLSMALLINT *out_len_chars) {
    std::vector<SQLWCHAR> wide;
    wide.reserve(str.size());
    for (size_t i = 0; i < str.size();) {
        unsigned char c = (unsigned char) str[i];
        unsigned int cp;
        size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < str.size()) {
            cp = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F); adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < str.size()) {
            cp = ((c & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) | (str[i + 2] & 0x3F); adv = 3;
        } else { cp = 0xFFFD; adv = 1; } // invalid/4-byte (astral) -- see comment above
        wide.push_back((SQLWCHAR) cp);
        i += adv;
    }
    if (out_len_chars) *out_len_chars = (SQLSMALLINT) wide.size();
    if (buffer == nullptr || buf_len_chars <= 0) return SQL_SUCCESS;
    bool truncated = wide.size() >= (size_t) buf_len_chars;
    size_t copy_n = truncated ? (size_t) buf_len_chars - 1 : wide.size();
    memcpy(buffer, wide.data(), copy_n * sizeof(SQLWCHAR));
    buffer[copy_n] = 0;
    return truncated ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

// Strips a trailing ';' and surrounding whitespace -- the engine's
// parser doesn't tolerate one (confirmed via direct isql testing: "SELECT
// COUNT(*) FROM t;" failed with "Only table name expected", the exact
// error this engine gives for unexpected trailing content). The JDBC
// driver's own CLI already did this stripping before ever reaching the
// engine; ODBC clients like isql send the semicolon through verbatim, so
// this driver needs to strip it itself.
static std::string StripTrailingSemicolon(const std::string &sql) {
    size_t end = sql.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return sql;
    if (sql[end] == ';') {
        end = (end == 0) ? std::string::npos : sql.find_last_not_of(" \t\r\n", end - 1);
        return (end == std::string::npos) ? "" : sql.substr(0, end + 1);
    }
    return sql.substr(0, end + 1);
}

extern "C" {

// ---------------------------------------------------------------------
// Handle allocation / freeing
// ---------------------------------------------------------------------

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE *output) {
    if (output == nullptr) return SQL_ERROR;
    switch (type) {
        case SQL_HANDLE_ENV:
            *output = new OdbcEnv();
            return SQL_SUCCESS;
        case SQL_HANDLE_DBC:
            *output = new OdbcConn();
            return SQL_SUCCESS;
        case SQL_HANDLE_STMT: {
            auto *stmt = new OdbcStmt();
            stmt->conn = (OdbcConn *) input;
            *output = stmt;
            return SQL_SUCCESS;
        }
        default:
            return SQL_ERROR;
    }
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT type, SQLHANDLE handle) {
    switch (type) {
        case SQL_HANDLE_ENV: delete (OdbcEnv *) handle; return SQL_SUCCESS;
        case SQL_HANDLE_DBC: delete (OdbcConn *) handle; return SQL_SUCCESS;
        case SQL_HANDLE_STMT: {
            auto *stmt = (OdbcStmt *) handle;
            if (stmt->prepared_plan && stmt->conn && stmt->conn->eng) {
                stmt->conn->eng->close_prepared(stmt->prepared_plan);
            }
            delete stmt;
            return SQL_SUCCESS;
        }
        default: return SQL_ERROR;
    }
}

SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT handle, SQLUSMALLINT option) {
    auto *stmt = (OdbcStmt *) handle;
    if (option == SQL_CLOSE) {
        stmt->has_result = false;
        stmt->result = dv1sql::query_result();
        stmt->current_row = -1;
    } else if (option == SQL_UNBIND) {
        stmt->bound_cols.clear();
    } else if (option == SQL_RESET_PARAMS) {
        stmt->bound_params.clear();
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER) {
    return SQL_SUCCESS; // accept anything (SQL_ATTR_ODBC_VERSION etc.) -- ODBC 3.x assumed throughout
}

// ---------------------------------------------------------------------
// Connect / disconnect
// ---------------------------------------------------------------------

// Core connect logic, NOT exported (no SQL_API/extern "C" linkage name
// collision risk) -- both SQLDriverConnect and SQLConnect call this
// directly as a plain C++ function. Confirmed as necessary, not
// defensive-programming excess: calling the EXPORTED SQLDriverConnect
// symbol from within SQLConnect resolved to the driver MANAGER's own
// same-named exported symbol (also loaded in the same process, since
// isql itself links libodbc.so) instead of this driver's own local
// function -- classic shared-library symbol interposition. Routing both
// public entry points through this private helper sidesteps the
// collision entirely.
static SQLRETURN DoConnect(OdbcConn *conn, const std::string &conn_str,
                            SQLCHAR *out_conn_str, SQLSMALLINT out_buf_len, SQLSMALLINT *out_len) {
    std::string path = ExtractDbq(conn_str);
    if (path.empty()) {
        SetError(conn->diag, "HY000", "Connection string must specify DBQ=<path to .mdsi file>");
        return SQL_ERROR;
    }

    conn->stm = std::unique_ptr<static_trie_map>(new static_trie_map());
    try {
        conn->stm->load(path.c_str());
    } catch (int errnum) {
        SetError(conn->diag, "08001", "Failed to open " + path + ": errno " + std::to_string(errnum));
        return SQL_ERROR;
    } catch (const std::exception &e) {
        SetError(conn->diag, "08001", "Failed to open " + path + ": " + e.what());
        return SQL_ERROR;
    } catch (...) {
        // An exception of ANY other type crossing the extern "C" boundary
        // uncaught is undefined behavior -- isql/any ODBC driver manager
        // is plain C with no exception handling at all. This catch-all
        // is required, not defensive-programming excess.
        SetError(conn->diag, "08001", "Failed to open " + path + ": unknown error");
        return SQL_ERROR;
    }
    conn->eng = std::unique_ptr<dv1sql::engine>(new dv1sql::engine());
    conn->eng->init(conn->stm.get());
    conn->connected = true;
    ClearError(conn->diag);

    if (out_conn_str && out_buf_len > 0) {
        std::string echoed = "DBQ=" + path;
        SQLSMALLINT n = (SQLSMALLINT) std::min((size_t) out_buf_len - 1, echoed.size());
        memcpy(out_conn_str, echoed.c_str(), n);
        out_conn_str[n] = '\0';
        if (out_len) *out_len = n;
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC handle, SQLHWND, SQLCHAR *in_conn_str, SQLSMALLINT,
                                    SQLCHAR *out_conn_str, SQLSMALLINT out_buf_len, SQLSMALLINT *out_len,
                                    SQLUSMALLINT) {
    auto *conn = (OdbcConn *) handle;
    std::string conn_str((const char *) in_conn_str);
    return DoConnect(conn, conn_str, out_conn_str, out_buf_len, out_len);
}

SQLRETURN SQL_API SQLConnect(SQLHDBC handle, SQLCHAR *dsn, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                              SQLCHAR *, SQLSMALLINT) {
    // Unlike SQLDriverConnect, SQLConnect receives only a bare DSN name --
    // no assembled connection string -- so the driver itself is
    // responsible for resolving the DSN's attributes from odbc.ini via
    // the standard SQLGetPrivateProfileString mechanism (odbcinst.h).
    // Confirmed via direct testing that this lookup is required: without
    // it, a DSN-based connect (isql -v <dsn>, the normal way to use a
    // named DSN) silently treated the DSN NAME itself as a file path and
    // failed, while DRIVER={Madras};DBQ=... direct connection strings
    // (which bypass DSN lookup entirely) worked correctly from the start.
    std::string dsn_str = dsn ? (const char *) dsn : "";
    char dbq[1024] = {0};
    SQLGetPrivateProfileString(dsn_str.c_str(), "DBQ", "", dbq, sizeof(dbq), "odbc.ini");
    std::string conn_str = std::string("DBQ=") + dbq;
    return DoConnect((OdbcConn *) handle, conn_str, nullptr, 0, nullptr);
}

SQLRETURN SQL_API SQLDisconnect(SQLHDBC handle) {
    auto *conn = (OdbcConn *) handle;
    conn->eng.reset();
    conn->stm.reset();
    conn->connected = false;
    return SQL_SUCCESS;
}

// ---------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT rec_number,
                                 SQLCHAR *sqlstate, SQLINTEGER *native_error,
                                 SQLCHAR *message, SQLSMALLINT buf_len, SQLSMALLINT *text_len) {
    if (rec_number != 1) return SQL_NO_DATA;
    OdbcDiag *diag = nullptr;
    if (type == SQL_HANDLE_DBC) diag = &((OdbcConn *) handle)->diag;
    else if (type == SQL_HANDLE_STMT) diag = &((OdbcStmt *) handle)->diag;
    if (diag == nullptr || !diag->has_error) return SQL_NO_DATA;

    if (sqlstate) memcpy(sqlstate, diag->sqlstate.c_str(), 6);
    if (native_error) *native_error = 0;
    if (message && buf_len > 0) {
        SQLSMALLINT n = (SQLSMALLINT) std::min((size_t) buf_len - 1, diag->message.size());
        memcpy(message, diag->message.c_str(), n);
        message[n] = '\0';
        if (text_len) *text_len = n;
    } else if (text_len) {
        *text_len = (SQLSMALLINT) diag->message.size();
    }
    return SQL_SUCCESS;
}

// ---------------------------------------------------------------------
// Execute (direct)
// ---------------------------------------------------------------------

// Core execute logic, NOT exported -- see DoConnect's own comment on why
// (symbol interposition risk when the same-named exported ODBC function
// is called internally from another entry point in this same driver).
static SQLRETURN DoExecDirect(OdbcStmt *stmt, const std::string &sql) {
    if (!stmt->conn || !stmt->conn->connected) {
        SetError(stmt->diag, "08003", "Connection is not open");
        return SQL_ERROR;
    }
    std::string sql_str = StripTrailingSemicolon(sql);
    stmt->result = stmt->conn->eng->execute(sql_str);
    stmt->has_result = true;
    stmt->current_row = -1;
    if (!stmt->result.ok) {
        SetError(stmt->diag, "42000", stmt->result.error);
        return SQL_ERROR;
    }
    ClearError(stmt->diag);
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLExecDirect(SQLHSTMT handle, SQLCHAR *sql, SQLINTEGER sql_len) {
    auto *stmt = (OdbcStmt *) handle;
    std::string sql_str = (sql_len == SQL_NTS) ? std::string((const char *) sql)
                                                 : std::string((const char *) sql, (size_t) sql_len);
    return DoExecDirect(stmt, sql_str);
}

// ---------------------------------------------------------------------
// Result set metadata
// ---------------------------------------------------------------------

SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT handle, SQLSMALLINT *count) {
    auto *stmt = (OdbcStmt *) handle;
    if (count) *count = stmt->has_result ? (SQLSMALLINT) stmt->result.column_names.size() : 0;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT handle, SQLUSMALLINT col_number,
                                  SQLCHAR *col_name, SQLSMALLINT buf_len, SQLSMALLINT *name_len,
                                  SQLSMALLINT *data_type, SQLULEN *col_size,
                                  SQLSMALLINT *decimal_digits, SQLSMALLINT *nullable) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->has_result || col_number < 1 || col_number > stmt->result.column_names.size()) {
        SetError(stmt->diag, "07009", "Invalid column number");
        return SQL_ERROR;
    }
    const std::string &name = stmt->result.column_names[col_number - 1];
    if (col_name && buf_len > 0) {
        SQLSMALLINT n = (SQLSMALLINT) std::min((size_t) buf_len - 1, name.size());
        memcpy(col_name, name.c_str(), n);
        col_name[n] = '\0';
        if (name_len) *name_len = n;
    } else if (name_len) {
        *name_len = (SQLSMALLINT) name.size();
    }
    // Every column is exposed as SQL_VARCHAR (see file header comment on
    // why) -- typed access still works via GetData/BindCol's on-demand
    // conversion to whatever C type the caller actually requests.
    if (data_type) *data_type = SQL_VARCHAR;
    if (col_size) *col_size = 1024;
    if (decimal_digits) *decimal_digits = 0;
    if (nullable) *nullable = SQL_NULLABLE_UNKNOWN;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColAttribute(SQLHSTMT handle, SQLUSMALLINT col_number, SQLUSMALLINT field_id,
                                   SQLPOINTER char_attr, SQLSMALLINT buf_len, SQLSMALLINT *str_len,
                                   SQLLEN *num_attr) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->has_result || col_number < 1 || col_number > stmt->result.column_names.size()) {
        return SQL_ERROR;
    }
    switch (field_id) {
        case SQL_DESC_NAME:
        case SQL_DESC_LABEL: {
            const std::string &name = stmt->result.column_names[col_number - 1];
            if (char_attr && buf_len > 0) {
                SQLSMALLINT n = (SQLSMALLINT) std::min((size_t) buf_len - 1, name.size());
                memcpy(char_attr, name.c_str(), n);
                ((char *) char_attr)[n] = '\0';
                if (str_len) *str_len = n;
            }
            return SQL_SUCCESS;
        }
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:
            if (num_attr) *num_attr = SQL_VARCHAR;
            return SQL_SUCCESS;
        case SQL_DESC_LENGTH:
        case SQL_DESC_OCTET_LENGTH:
            if (num_attr) *num_attr = 1024;
            return SQL_SUCCESS;
        case SQL_DESC_NULLABLE:
            if (num_attr) *num_attr = SQL_NULLABLE_UNKNOWN;
            return SQL_SUCCESS;
        default:
            if (num_attr) *num_attr = 0;
            return SQL_SUCCESS;
    }
}

// ---------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------

SQLRETURN SQL_API SQLFetch(SQLHSTMT handle) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->has_result) return SQL_ERROR;
    stmt->current_row++;
    if ((size_t) stmt->current_row >= stmt->result.rows.size()) {
        stmt->current_row = (long long) stmt->result.rows.size(); // past the end
        return SQL_NO_DATA;
    }
    // Populate any SQLBindCol-bound buffers automatically -- unlike
    // GetData, bound columns refresh on every Fetch without the caller
    // asking again.
    const auto &row = stmt->result.rows[stmt->current_row];
    for (size_t i = 0; i < stmt->bound_cols.size(); i++) {
        BoundCol &bc = stmt->bound_cols[i];
        if (!bc.bound || i >= row.size()) continue;
        bool is_null = row[i].empty(); // this engine has no separate NULL
                                        // marker in query_result -- an
                                        // empty string is the closest
                                        // available signal, a known,
                                        // documented simplification (see
                                        // README) shared with the rest of
                                        // this driver's text-based model.
        CopyToBuffer(&row[i], is_null, bc.c_type, bc.app_buffer, bc.buffer_len, bc.str_len_or_ind);
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetData(SQLHSTMT handle, SQLUSMALLINT col_number, SQLSMALLINT c_type,
                              SQLPOINTER buffer, SQLLEN buffer_len, SQLLEN *str_len_or_ind) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->has_result || stmt->current_row < 0 ||
        (size_t) stmt->current_row >= stmt->result.rows.size()) {
        SetError(stmt->diag, "24000", "No current row");
        return SQL_ERROR;
    }
    const auto &row = stmt->result.rows[stmt->current_row];
    if (col_number < 1 || col_number > row.size()) {
        SetError(stmt->diag, "07009", "Invalid column number");
        return SQL_ERROR;
    }
    const std::string &value = row[col_number - 1];
    bool is_null = value.empty();
    return CopyToBuffer(&value, is_null, c_type, buffer, buffer_len, str_len_or_ind);
}

SQLRETURN SQL_API SQLBindCol(SQLHSTMT handle, SQLUSMALLINT col_number, SQLSMALLINT c_type,
                              SQLPOINTER buffer, SQLLEN buffer_len, SQLLEN *str_len_or_ind) {
    auto *stmt = (OdbcStmt *) handle;
    if (col_number < 1) return SQL_ERROR;
    if ((size_t) col_number > stmt->bound_cols.size()) {
        stmt->bound_cols.resize(col_number);
    }
    BoundCol &bc = stmt->bound_cols[col_number - 1];
    if (buffer == nullptr) {
        bc.bound = false;
    } else {
        bc.bound = true;
        bc.c_type = c_type;
        bc.app_buffer = buffer;
        bc.buffer_len = buffer_len;
        bc.str_len_or_ind = str_len_or_ind;
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLRowCount(SQLHSTMT handle, SQLLEN *count) {
    auto *stmt = (OdbcStmt *) handle;
    if (count) *count = stmt->has_result ? (SQLLEN) stmt->result.rows.size() : 0;
    return SQL_SUCCESS;
}

// ---------------------------------------------------------------------
// Prepare / bind parameters / execute -- reuses dv1sql::engine's own
// prepare()/execute_prepared() directly (the same mechanism the JDBC
// PreparedStatement uses), not a re-implementation.
// ---------------------------------------------------------------------

static SQLRETURN DoPrepare(OdbcStmt *stmt, const std::string &sql) {
    if (!stmt->conn || !stmt->conn->connected) {
        SetError(stmt->diag, "08003", "Connection is not open");
        return SQL_ERROR;
    }
    if (stmt->prepared_plan) {
        stmt->conn->eng->close_prepared(stmt->prepared_plan);
        stmt->prepared_plan = nullptr;
    }
    stmt->sql_text = StripTrailingSemicolon(sql);
    std::string error;
    stmt->prepared_plan = stmt->conn->eng->prepare(stmt->sql_text, error);
    if (stmt->prepared_plan == nullptr) {
        SetError(stmt->diag, "42000", error);
        return SQL_ERROR;
    }
    stmt->param_count = stmt->conn->eng->param_count(stmt->prepared_plan);
    stmt->bound_params.assign(stmt->param_count, BoundParam());
    ClearError(stmt->diag);
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLPrepare(SQLHSTMT handle, SQLCHAR *sql, SQLINTEGER sql_len) {
    auto *stmt = (OdbcStmt *) handle;
    std::string sql_str = (sql_len == SQL_NTS) ? std::string((const char *) sql)
                                                 : std::string((const char *) sql, (size_t) sql_len);
    return DoPrepare(stmt, sql_str);
}

SQLRETURN SQL_API SQLBindParameter(SQLHSTMT handle, SQLUSMALLINT param_number, SQLSMALLINT /*io_type*/,
                                    SQLSMALLINT c_type, SQLSMALLINT /*sql_type*/, SQLULEN /*col_size*/,
                                    SQLSMALLINT /*decimal_digits*/, SQLPOINTER buffer, SQLLEN buffer_len,
                                    SQLLEN *str_len_or_ind) {
    auto *stmt = (OdbcStmt *) handle;
    if (param_number < 1) {
        SetError(stmt->diag, "07009", "Invalid parameter number");
        return SQL_ERROR;
    }
    if ((size_t) param_number > stmt->bound_params.size()) {
        stmt->bound_params.resize(param_number);
    }
    BoundParam &p = stmt->bound_params[param_number - 1];
    p.c_type = c_type;
    p.app_buffer = buffer;
    p.buffer_len = buffer_len;
    p.str_len_or_ind = str_len_or_ind;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLExecute(SQLHSTMT handle) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->prepared_plan) {
        SetError(stmt->diag, "HY010", "Statement was not prepared -- call SQLPrepare first");
        return SQL_ERROR;
    }
    std::vector<std::string> params;
    params.reserve(stmt->bound_params.size());
    for (auto &p : stmt->bound_params) {
        params.push_back(p.app_buffer ? ParamToString(p) : "");
    }
    stmt->result = stmt->conn->eng->execute_prepared(stmt->prepared_plan, params);
    stmt->has_result = true;
    stmt->current_row = -1;
    if (!stmt->result.ok) {
        SetError(stmt->diag, "42000", stmt->result.error);
        return SQL_ERROR;
    }
    ClearError(stmt->diag);
    return SQL_SUCCESS;
}

// ---------------------------------------------------------------------
// Connection/statement attributes -- safe no-ops. Many real client tools
// (not just isql) call these unconditionally during connect/setup and
// expect SQL_SUCCESS even though this driver has nothing meaningful to
// configure (no transactions, no cursor types beyond forward-only, no
// autocommit toggle since there's no write path at all).
// ---------------------------------------------------------------------

// Driver-specific connection attribute, exposed so ODBC clients (like the
// CLI tool below) can toggle "Using index: ..." logging without needing
// C++-level access to the engine directly -- ODBC's standard mechanism
// for driver-specific behavior is exactly this: a custom attribute number
// outside the reserved standard range, handled via SetConnectAttr.
#define MADRAS_ATTR_INDEX_LOGGING 12001

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER) {
    if (attr == MADRAS_ATTR_INDEX_LOGGING) {
        dv1sql::index_logging_enabled() = (value != nullptr) && (((intptr_t) value) != 0);
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC, SQLINTEGER attr, SQLPOINTER value,
                                     SQLINTEGER, SQLINTEGER *) {
    if (value == nullptr) return SQL_SUCCESS;
    switch (attr) {
        case SQL_ATTR_AUTOCOMMIT: *(SQLUINTEGER *) value = SQL_AUTOCOMMIT_ON; break;
        case SQL_ATTR_TXN_ISOLATION: *(SQLUINTEGER *) value = SQL_TXN_READ_COMMITTED; break;
        default: *(SQLUINTEGER *) value = 0; break;
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER) {
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER, SQLINTEGER *) {
    if (value == nullptr) return SQL_SUCCESS;
    if (attr == SQL_ATTR_CURSOR_TYPE) *(SQLUINTEGER *) value = SQL_CURSOR_FORWARD_ONLY;
    else *(SQLUINTEGER *) value = 0;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLEndTran(SQLSMALLINT, SQLHANDLE, SQLSMALLINT) {
    return SQL_SUCCESS; // no transactions -- every statement is effectively auto-committed
}

// ---------------------------------------------------------------------
// SQLGetInfo -- answers the commonly-probed info types with sane,
// standards-conformant values. Many client tools (drivers-manager UIs,
// pyodbc, BI tools) call this before even attempting a query and can
// refuse to proceed on an unrecognized/unhandled type; unhandled types
// here fall through to a zeroed/empty default rather than failing, which
// is the conservative, safe choice.
// ---------------------------------------------------------------------

static void CopyInfoString(const char *s, SQLPOINTER buf, SQLSMALLINT buf_len, SQLSMALLINT *out_len) {
    std::string str(s);
    if (buf && buf_len > 0) {
        SQLSMALLINT n = (SQLSMALLINT) std::min((size_t) buf_len - 1, str.size());
        memcpy(buf, str.c_str(), n);
        ((char *) buf)[n] = '\0';
    }
    if (out_len) *out_len = (SQLSMALLINT) str.size();
}

static SQLRETURN DoGetInfo(SQLUSMALLINT info_type, SQLPOINTER value,
                            SQLSMALLINT buf_len, SQLSMALLINT *out_len) {
    switch (info_type) {
        case SQL_DRIVER_NAME: CopyInfoString("libmadras_odbc.so", value, buf_len, out_len); break;
        case SQL_DBMS_NAME: CopyInfoString("Madras", value, buf_len, out_len); break;
        case SQL_DBMS_VER: CopyInfoString("01.00.0000", value, buf_len, out_len); break;
        case SQL_DRIVER_VER: CopyInfoString("01.00.0000", value, buf_len, out_len); break;
        case SQL_DRIVER_ODBC_VER: CopyInfoString("03.80", value, buf_len, out_len); break;
        case SQL_IDENTIFIER_QUOTE_CHAR: CopyInfoString("\"", value, buf_len, out_len); break;
        case SQL_SEARCH_PATTERN_ESCAPE: CopyInfoString("", value, buf_len, out_len); break;
        case SQL_CATALOG_NAME_SEPARATOR: CopyInfoString(".", value, buf_len, out_len); break;
        case SQL_TABLE_TERM: CopyInfoString("table", value, buf_len, out_len); break;
        case SQL_USER_NAME: CopyInfoString("", value, buf_len, out_len); break;
        case SQL_DATA_SOURCE_READ_ONLY: CopyInfoString("Y", value, buf_len, out_len); break;

        // Numeric (SQLUSMALLINT) info types
        case SQL_MAX_COLUMN_NAME_LEN:
        case SQL_MAX_TABLE_NAME_LEN:
        case SQL_MAX_CATALOG_NAME_LEN:
        case SQL_MAX_SCHEMA_NAME_LEN:
            if (value) *(SQLUSMALLINT *) value = 128;
            break;
        case SQL_TXN_CAPABLE:
            if (value) *(SQLUSMALLINT *) value = SQL_TC_NONE; // no transactions
            break;
        case SQL_CURSOR_COMMIT_BEHAVIOR:
        case SQL_CURSOR_ROLLBACK_BEHAVIOR:
            if (value) *(SQLUSMALLINT *) value = SQL_CB_PRESERVE;
            break;

        // Numeric (SQLUINTEGER bitmask) info types
        case SQL_GETDATA_EXTENSIONS:
            if (value) *(SQLUINTEGER *) value = SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER;
            break;
        case SQL_TXN_ISOLATION_OPTION:
            if (value) *(SQLUINTEGER *) value = SQL_TXN_READ_COMMITTED;
            break;
        case SQL_SCROLL_OPTIONS:
            if (value) *(SQLUINTEGER *) value = SQL_SO_FORWARD_ONLY;
            break;
        default:
            if (value) *(SQLUINTEGER *) value = 0;
            if (out_len) *out_len = 0;
            break;
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetInfo(SQLHDBC, SQLUSMALLINT info_type, SQLPOINTER value,
                              SQLSMALLINT buf_len, SQLSMALLINT *out_len) {
    return DoGetInfo(info_type, value, buf_len, out_len);
}

// ---------------------------------------------------------------------
// Catalog functions -- minimal SQLTables/SQLColumns, enough for schema
// browsing (many GUI/BI tools require these to work at all before they'll
// let a user pick a table/column). Reads directly from the data_source's
// own column() metadata, not through the SQL parser -- no query text
// involved.
// ---------------------------------------------------------------------

static SQLRETURN DoTables(OdbcStmt *stmt) {
    stmt->result = dv1sql::query_result();
    stmt->result.ok = true;
    stmt->result.column_names = {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"};
    stmt->result.rows.push_back({"", "", "t", "TABLE", ""});
    stmt->has_result = true;
    stmt->current_row = -1;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLTables(SQLHSTMT handle, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                             SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT) {
    return DoTables((OdbcStmt *) handle);
}

static SQLRETURN DoColumns(OdbcStmt *stmt) {
    stmt->result = dv1sql::query_result();
    stmt->result.ok = true;
    stmt->result.column_names = {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME",
                                  "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE", "NULLABLE"};
    if (stmt->conn && stmt->conn->eng) {
        // trie_data_source's own column metadata, exposed via a tiny
        // engine-side accessor -- see engine.hpp's column_count()/
        // column_name() additions.
        uint32_t n = stmt->conn->eng->column_count();
        for (uint32_t i = 0; i < n; i++) {
            stmt->result.rows.push_back({"", "", "t", stmt->conn->eng->column_name(i),
                                          std::to_string(SQL_VARCHAR), "VARCHAR", "1024", "2"});
        }
    }
    stmt->has_result = true;
    stmt->current_row = -1;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT handle, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                              SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT) {
    return DoColumns((OdbcStmt *) handle);
}

// ---------------------------------------------------------------------
// Unicode entry points -- convert to/from UTF-8 (see Utf16ToUtf8/
// Utf8ToUtf16Buffer above) and delegate to the SAME private, non-exported
// core-logic helpers the ANSI entry points use (DoConnect/DoExecDirect/
// DoPrepare), not to the exported ANSI symbols themselves -- avoids the
// same symbol-interposition hazard fixed earlier for SQLConnect/
// SQLDriverConnect.
// ---------------------------------------------------------------------

SQLRETURN SQL_API SQLDriverConnectW(SQLHDBC handle, SQLHWND, SQLWCHAR *in_conn_str, SQLSMALLINT in_len,
                                     SQLWCHAR *out_conn_str, SQLSMALLINT out_buf_len_chars, SQLSMALLINT *out_len_chars,
                                     SQLUSMALLINT) {
    auto *conn = (OdbcConn *) handle;
    std::string conn_str = Utf16ToUtf8(in_conn_str, in_len == SQL_NTS ? SQL_NTS : in_len);
    // DoConnect's own out_conn_str echo is ANSI (SQLCHAR*) -- build it
    // narrow, then convert once for the W caller, rather than teaching
    // DoConnect two output encodings.
    std::vector<char> ansi_out(out_buf_len_chars > 0 ? out_buf_len_chars : 256);
    SQLSMALLINT ansi_out_len = 0;
    SQLRETURN r = DoConnect(conn, conn_str, (SQLCHAR *) ansi_out.data(), (SQLSMALLINT) ansi_out.size(), &ansi_out_len);
    if ((r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) && out_conn_str) {
        Utf8ToUtf16Buffer(std::string(ansi_out.data()), out_conn_str, out_buf_len_chars, out_len_chars);
    }
    return r;
}

SQLRETURN SQL_API SQLConnectW(SQLHDBC handle, SQLWCHAR *dsn, SQLSMALLINT dsn_len, SQLWCHAR *, SQLSMALLINT,
                               SQLWCHAR *, SQLSMALLINT) {
    auto *conn = (OdbcConn *) handle;
    std::string dsn_str = Utf16ToUtf8(dsn, dsn_len);
    char dbq[1024] = {0};
    SQLGetPrivateProfileString(dsn_str.c_str(), "DBQ", "", dbq, sizeof(dbq), "odbc.ini");
    std::string conn_str = std::string("DBQ=") + dbq;
    return DoConnect(conn, conn_str, nullptr, 0, nullptr);
}

SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT handle, SQLWCHAR *sql, SQLINTEGER sql_len) {
    auto *stmt = (OdbcStmt *) handle;
    return DoExecDirect(stmt, Utf16ToUtf8(sql, sql_len));
}

SQLRETURN SQL_API SQLPrepareW(SQLHSTMT handle, SQLWCHAR *sql, SQLINTEGER sql_len) {
    auto *stmt = (OdbcStmt *) handle;
    return DoPrepare(stmt, Utf16ToUtf8(sql, sql_len));
}

SQLRETURN SQL_API SQLGetDataW(SQLHSTMT handle, SQLUSMALLINT col_number, SQLSMALLINT c_type,
                               SQLPOINTER buffer, SQLLEN buffer_len, SQLLEN *str_len_or_ind) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->has_result || stmt->current_row < 0 ||
        (size_t) stmt->current_row >= stmt->result.rows.size()) {
        SetError(stmt->diag, "24000", "No current row");
        return SQL_ERROR;
    }
    const auto &row = stmt->result.rows[stmt->current_row];
    if (col_number < 1 || col_number > row.size()) {
        SetError(stmt->diag, "07009", "Invalid column number");
        return SQL_ERROR;
    }
    const std::string &value = row[col_number - 1];
    // Default to SQL_C_WCHAR for a Unicode caller that didn't specify
    // (SQL_C_DEFAULT) -- CopyToBuffer now handles SQL_C_WCHAR correctly
    // (see its own comment on why this case is required, not optional).
    SQLSMALLINT effective_type = (c_type == SQL_C_DEFAULT) ? SQL_C_WCHAR : c_type;
    return CopyToBuffer(&value, value.empty(), effective_type, buffer, buffer_len, str_len_or_ind);
}

SQLRETURN SQL_API SQLDescribeColW(SQLHSTMT handle, SQLUSMALLINT col_number,
                                   SQLWCHAR *col_name, SQLSMALLINT buf_len_chars, SQLSMALLINT *name_len_chars,
                                   SQLSMALLINT *data_type, SQLULEN *col_size,
                                   SQLSMALLINT *decimal_digits, SQLSMALLINT *nullable) {
    auto *stmt = (OdbcStmt *) handle;
    if (!stmt->has_result || col_number < 1 || col_number > stmt->result.column_names.size()) {
        SetError(stmt->diag, "07009", "Invalid column number");
        return SQL_ERROR;
    }
    const std::string &name = stmt->result.column_names[col_number - 1];
    Utf8ToUtf16Buffer(name, col_name, buf_len_chars, name_len_chars);
    if (data_type) *data_type = SQL_VARCHAR;
    if (col_size) *col_size = 1024;
    if (decimal_digits) *decimal_digits = 0;
    if (nullable) *nullable = SQL_NULLABLE_UNKNOWN;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT rec_number,
                                  SQLWCHAR *sqlstate, SQLINTEGER *native_error,
                                  SQLWCHAR *message, SQLSMALLINT buf_len_chars, SQLSMALLINT *text_len_chars) {
    if (rec_number != 1) return SQL_NO_DATA;
    OdbcDiag *diag = nullptr;
    if (type == SQL_HANDLE_DBC) diag = &((OdbcConn *) handle)->diag;
    else if (type == SQL_HANDLE_STMT) diag = &((OdbcStmt *) handle)->diag;
    if (diag == nullptr || !diag->has_error) return SQL_NO_DATA;

    if (sqlstate) Utf8ToUtf16Buffer(diag->sqlstate, sqlstate, 6, nullptr);
    if (native_error) *native_error = 0;
    Utf8ToUtf16Buffer(diag->message, message, buf_len_chars, text_len_chars);
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetInfoW(SQLHDBC handle, SQLUSMALLINT info_type, SQLPOINTER value,
                               SQLSMALLINT buf_len_chars, SQLSMALLINT *out_len_chars) {
    // Numeric info types need no conversion -- reuse the ANSI path's
    // logic wholesale for those by writing into a throwaway ANSI buffer
    // only when the type is string-valued.
    static const SQLUSMALLINT string_types[] = {
        SQL_DRIVER_NAME, SQL_DBMS_NAME, SQL_DBMS_VER, SQL_DRIVER_VER, SQL_DRIVER_ODBC_VER,
        SQL_IDENTIFIER_QUOTE_CHAR, SQL_SEARCH_PATTERN_ESCAPE, SQL_CATALOG_NAME_SEPARATOR,
        SQL_TABLE_TERM, SQL_USER_NAME, SQL_DATA_SOURCE_READ_ONLY
    };
    bool is_string = false;
    for (auto t : string_types) if (t == info_type) { is_string = true; break; }
    if (!is_string) {
        return DoGetInfo(info_type, value, 0, nullptr);
    }
    char ansi_buf[256] = {0};
    SQLSMALLINT ansi_len = 0;
    DoGetInfo(info_type, ansi_buf, sizeof(ansi_buf), &ansi_len);
    return Utf8ToUtf16Buffer(std::string(ansi_buf), (SQLWCHAR *) value, buf_len_chars, out_len_chars);
}

SQLRETURN SQL_API SQLTablesW(SQLHSTMT handle, SQLWCHAR *, SQLSMALLINT, SQLWCHAR *, SQLSMALLINT,
                              SQLWCHAR *, SQLSMALLINT, SQLWCHAR *, SQLSMALLINT) {
    return DoTables((OdbcStmt *) handle);
}

SQLRETURN SQL_API SQLColumnsW(SQLHSTMT handle, SQLWCHAR *, SQLSMALLINT, SQLWCHAR *, SQLSMALLINT,
                               SQLWCHAR *, SQLSMALLINT, SQLWCHAR *, SQLSMALLINT) {
    return DoColumns((OdbcStmt *) handle);
}

} // extern "C"
