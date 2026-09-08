# madras_odbc

A minimal, genuinely usable ODBC driver around the `dv1sql::engine` C++
SQL engine (`madras_sql_repo`) -- no new SQL logic here, purely a C-ABI
protocol adapter, in the same spirit as the JNI layer in `madras_jvm`.

Scoped deliberately narrow, not a fully spec-conformant driver: enough for
real tools (`isql`, `pyodbc`, Excel/Power BI's generic ODBC connector) to
connect and run `SELECT` queries against a `.mdsi` file. No DML, no
transactions, no Unicode (`SQLWCHAR`) entry points, no scrollable cursors.

**Verified end-to-end** against a real unixODBC 2.3.12 install (real
`isql`, real driver manager, real `odbcinst.ini`/`odbc.ini`) during
development -- connect (both DSN-based `SQLConnect` and DSN-less
`SQLDriverConnect`), `SQLPrepare`/`SQLBindParameter`/`SQLExecute`,
`SQLExecDirect`, `SQLFetch`/`SQLGetData`/`SQLBindCol`, `SQLTables`/
`SQLColumns` (via `isql`'s own `help`/`help <table>` commands -- note:
unixODBC's `isql` uses `help`, not `!tables`/`!columns`, which is iODBC's
syntax instead), and result correctness (`COUNT`, `SUM`, indexed `WHERE`,
multi-condition `AND`, all cross-checked against previously-verified
values) all confirmed working, not just compiled. Windows itself was not
tested (no Windows environment available) -- see the Windows section
below for what to expect/verify.

## Building

```bash
cmake -B build -DMADRAS_INCLUDE_DIR=/path/to/madras-trie/include \
               -DMADRAS_SQL_INCLUDE_DIR=/path/to/madras_sql_repo/src
cmake --build build
```

Produces `build/libmadras_odbc.so` (Linux), `.dylib` (macOS), or
`madras_odbc.dll` (Windows).

## Linux / macOS setup (unixODBC)

Install unixODBC first if you don't have it:
```bash
# Debian/Ubuntu
sudo apt-get install unixodbc unixodbc-dev
# macOS
brew install unixodbc
```

Register the driver (`odbcinst.ini`, typically `/etc/odbcinst.ini` or
`~/.odbcinst.ini`):
```ini
[Madras]
Description=Madras SQL Engine ODBC Driver
Driver=/path/to/libmadras_odbc.so
Setup=/path/to/libmadras_odbc.so
FileUsage=1
```

Then define a data source (`odbc.ini`, typically `/etc/odbc.ini` or
`~/.odbc.ini`):
```ini
[MyData]
Description=My madras data source
Driver=Madras
DBQ=/path/to/your/file.mdsi
```

Test it:
```bash
isql -v MyData
SQL> SELECT COUNT(*) FROM t;
```

You can also connect without a named DSN at all, specifying the driver
and path directly in the connection string (useful for scripting, or
tools that don't want a system-wide DSN registration):
```
DRIVER={Madras};DBQ=/path/to/your/file.mdsi;
```

## Windows setup

Not tested in the environment that built this (no Windows available) --
the ODBC API surface used here is standard ODBC 3.x, so the following
*should* work, but please verify it for real before relying on it:

1. Build `madras_odbc.dll` with MSVC or MinGW, linking against the
   Windows SDK's `odbc32.lib`/`odbccp32.lib` (no separate unixODBC-style
   package needed -- the driver manager is a built-in Windows component).
2. Register the driver via the **ODBC Data Source Administrator**
   (`odbcad32.exe`) -- Add a driver pointing at `madras_odbc.dll` -- or
   programmatically via `SQLInstallDriverEx`.
3. Add a DSN (System or User) through the same Administrator UI, or a
   DSN-less connection string identical in form to the Linux/macOS one
   above.

## Known simplifications (by design, for this scope)

- Every column is exposed as `SQL_VARCHAR` regardless of its real type --
  matches the engine's own already-text-formatted `query_result`. Typed
  access still works: `SQLGetData`/`SQLBindCol` convert on demand to
  whatever C type (`SQL_C_LONG`, `SQL_C_DOUBLE`, etc.) the caller actually
  requests.
- No separate `NULL` marker in the underlying `query_result` -- an empty
  string is the closest available signal, consistent with the rest of
  this project's text-based value model (same simplification the JDBC
  driver makes).
- Read-only: no `INSERT`/`UPDATE`/`DELETE`, no transactions (`SQLEndTran`
  is a no-op; there's nothing to commit or roll back).
- `SQLGetInfo` answers the commonly-probed info types with sane defaults;
  anything not explicitly handled falls through to a zeroed/empty
  default rather than failing, so unrecognized probes don't block a tool
  from proceeding.
