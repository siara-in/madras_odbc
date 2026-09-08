cmake_minimum_required(VERSION 3.10)
project(madras_odbc)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Same convention as madras_jvm/native/CMakeLists.txt -- override with
# -DMADRAS_INCLUDE_DIR=/path/to/madras-trie/include if your checkout
# layout differs.
if(NOT DEFINED MADRAS_INCLUDE_DIR)
  set(MADRAS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/madras-trie/include")
endif()

# Same convention -- override with -DMADRAS_SQL_INCLUDE_DIR=/path/to/madras_sql_repo/src
if(NOT DEFINED MADRAS_SQL_INCLUDE_DIR)
  set(MADRAS_SQL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../madras_sql_repo/src")
endif()

find_package(Threads REQUIRED)

# unixODBC on Linux/macOS. On Windows, the platform ODBC SDK is used
# instead (sql.h/sqlext.h/sqltypes.h ship with the Windows SDK; no
# separate driver-manager package needed there -- see README.md).
#
# /opt/homebrew is Homebrew's prefix on Apple Silicon (M1/M2/etc.);
# /usr/local is Homebrew's prefix on Intel Macs. Both are searched since
# either could be in play depending on the machine -- confirmed necessary
# after this CMakeLists.txt's find_path/find_library calls initially only
# checked /usr/local and would have missed unixODBC entirely on Apple
# Silicon otherwise.
if(NOT WIN32)
  find_path(ODBC_INCLUDE_DIR sql.h PATHS /usr/include /usr/local/include /opt/homebrew/include)
  find_library(ODBC_LIBRARY NAMES odbc odbcinst PATHS /usr/lib /usr/local/lib /opt/homebrew/lib)
  find_library(ODBCINST_LIBRARY NAMES odbcinst PATHS /usr/lib /usr/local/lib /opt/homebrew/lib)
  if(NOT ODBC_INCLUDE_DIR)
    message(FATAL_ERROR "unixODBC headers not found -- install unixodbc-dev (Debian/Ubuntu) or unixODBC (Homebrew: brew install unixodbc)")
  endif()
endif()

add_library(madras_odbc SHARED src/madras_odbc_driver.cpp)

target_include_directories(madras_odbc PRIVATE
  ${MADRAS_INCLUDE_DIR}
  ${MADRAS_SQL_INCLUDE_DIR}
  ${ODBC_INCLUDE_DIR}
)

target_link_libraries(madras_odbc PRIVATE Threads::Threads)
if(NOT WIN32)
  target_link_libraries(madras_odbc PRIVATE ${ODBCINST_LIBRARY})
endif()

target_compile_options(madras_odbc PRIVATE -O3)

# Output name matches what odbcinst.ini.template / the README expect:
# libmadras_odbc.so (Linux), libmadras_odbc.dylib (macOS), madras_odbc.dll
# (Windows, via the DLL suffix CMake picks automatically there).
set_target_properties(madras_odbc PROPERTIES
  OUTPUT_NAME "madras_odbc"
  PREFIX "lib"
)

# --------------------------------------------------------------------
# odbc_cli: the timing-aware command-line tool (plain C, not part of the
# driver itself -- links against the system ODBC driver MANAGER, like any
# ODBC client application would, not against madras_odbc directly).
# Requires GNU readline; on macOS, Homebrew's readline isn't linked into
# the default include/library search path even when installed (it's
# "keg-only" to avoid conflicting with Apple's own libedit-based
# readline-compatibility shim), so its Homebrew prefix is added
# explicitly here rather than assumed to already be on the path.
# --------------------------------------------------------------------
if(NOT WIN32)
  find_path(READLINE_INCLUDE_DIR readline/readline.h
    PATHS /usr/include /usr/local/include /opt/homebrew/include
          /usr/local/opt/readline/include /opt/homebrew/opt/readline/include)
  find_library(READLINE_LIBRARY NAMES readline
    PATHS /usr/lib /usr/local/lib /opt/homebrew/lib
          /usr/local/opt/readline/lib /opt/homebrew/opt/readline/lib)

  if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
    add_executable(odbc_cli tools/odbc_cli.c)
    target_include_directories(odbc_cli PRIVATE ${ODBC_INCLUDE_DIR} ${READLINE_INCLUDE_DIR})
    target_link_libraries(odbc_cli PRIVATE ${ODBC_LIBRARY} ${READLINE_LIBRARY})
  else()
    message(WARNING "GNU readline not found -- odbc_cli target skipped. "
                     "Install it (apt: libreadline-dev, brew: readline) and re-run cmake to build it.")
  endif()
endif()
