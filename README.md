# Madras ODBC

An ODBC driver for running SQL `SELECT` queries against Madras Sorcery `.mdsi` files from any ODBC-capable tool — `isql`, `pyodbc`, Excel, Power BI's generic ODBC connector, and more.

## Madras Sorcery

Madras Sorcery is a compact, static datastore where a single `.mdsi` file is simultaneously a compressed column store *and* a sorted, directly-navigable index — no separate index file, no decompress-then-scan step. See the [madras_sorcery](https://github.com/siara-in/madras_sorcery) super-repo for the full project overview.

## Getting started

This repository vendors [madras_sorcery_core](https://github.com/siara-in/madras_sorcery_core) and [madras_sql](https://github.com/siara-in/madras_sql) as git submodules, so clone with:

```bash
git clone --recursive https://github.com/siara-in/madras_odbc.git
# or, if already cloned:
git submodule update --init --recursive
```

Then build the driver:

```bash
cmake -B build
cmake --build build
```

To build against a different checkout of either dependency instead of the submodules, pass `-DMADRAS_INCLUDE_DIR=/path/to/include` and/or `-DMADRAS_SQL_INCLUDE_DIR=/path/to/src`.

Produces `build/libmadras_odbc.so` (Linux), `.dylib` (macOS), or `madras_odbc.dll` (Windows).

### Linux / macOS setup (unixODBC)

```bash
# Debian/Ubuntu
sudo apt-get install unixodbc unixodbc-dev
# macOS
brew install unixodbc
```

Register the driver in `odbcinst.ini` (typically `/etc/odbcinst.ini` or `~/.odbcinst.ini`):

```ini
[Madras]
Description=Madras SQL Engine ODBC Driver
Driver=/path/to/libmadras_odbc.so
Setup=/path/to/libmadras_odbc.so
FileUsage=1
```

Define a data source in `odbc.ini` (typically `/etc/odbc.ini` or `~/.odbc.ini`):

```ini
[MyData]
Description=My madras data source
Driver=Madras
DBQ=/path/to/your/file.mdsi
```

Then connect and query:

```bash
isql -v MyData
SQL> SELECT COUNT(*) FROM t;
```

You can also skip DSN registration entirely and specify the driver and file path directly in a connection string:

```
DRIVER={Madras};DBQ=/path/to/your/file.mdsi;
```

See [`odbc.ini.template`](odbc.ini.template) and [`odbcinst.ini.template`](odbcinst.ini.template) for Windows setup and further detail. Note this is a deliberately scoped driver — `SELECT` queries only, no DML, transactions, Unicode entry points, or scrollable cursors.

## License

This work is licensed under the MIT License. See [LICENSE](LICENSE).

## Support

Please feel free to communicate suggestions, improvements and corrections by creating issues here or send email to Arundale Ramanathan at arun@siara.in.
