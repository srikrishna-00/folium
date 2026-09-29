# ---------------------------------------------------------------------------
# Locate SQLite and *prove* the found library has FTS5 compiled in.
#
# The search module (database-design.md section 3.9) commits us to FTS5 for
# local full-text search. A platform libsqlite3 without FTS5 would not fail at
# link time -- it would fail at runtime, on the user's device, the first time
# anyone searched. So we compile-and-run a probe at configure time instead of
# assuming. Spec section 5: never claim a feature works unless it is tested.
#
# Produces the imported target: uapp::sqlite3
# ---------------------------------------------------------------------------

include(CheckCXXSourceRuns)

find_package(SQLite3 REQUIRED)

set(_uapp_fts5_probe [[
#include <sqlite3.h>
int main(void) {
    sqlite3* db = 0;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    int rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE probe USING fts5(a, tokenize='unicode61 remove_diacritics 2', prefix='2 3');",
        0, 0, 0);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : 2;
}
]])

set(CMAKE_REQUIRED_INCLUDES   "${SQLite3_INCLUDE_DIRS}")
set(CMAKE_REQUIRED_LIBRARIES  "${SQLite3_LIBRARIES}")
check_cxx_source_runs("${_uapp_fts5_probe}" UAPP_SQLITE3_HAS_FTS5)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

if(NOT UAPP_SQLITE3_HAS_FTS5)
    message(FATAL_ERROR
        "The SQLite found at ${SQLite3_LIBRARIES} (version ${SQLite3_VERSION}) does not "
        "support FTS5, which core/notes search requires. Install a SQLite built with "
        "-DSQLITE_ENABLE_FTS5, or point CMake at one via SQLite3_ROOT.")
endif()

add_library(uapp_sqlite3 INTERFACE)
add_library(uapp::sqlite3 ALIAS uapp_sqlite3)
# SQLite3::SQLite3 on CMake >= 3.27; SQLite::SQLite3 is the deprecated spelling
# still used by older CMake's FindSQLite3.
if(TARGET SQLite3::SQLite3)
    target_link_libraries(uapp_sqlite3 INTERFACE SQLite3::SQLite3)
else()
    target_link_libraries(uapp_sqlite3 INTERFACE SQLite::SQLite3)
endif()

message(STATUS "uapp: SQLite ${SQLite3_VERSION} (FTS5: yes) at ${SQLite3_LIBRARIES}")
