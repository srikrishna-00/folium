add_library(uapp_storage_sqlite STATIC
    src/fts_query.cpp
    src/search_index_sql.cpp
    src/sqlite_block_store.cpp
    src/sqlite_database_store.cpp
    src/sqlite_note_store.cpp
    src/sqlite_search_index.cpp)
add_library(uapp::storage_sqlite ALIAS uapp_storage_sqlite)

target_include_directories(uapp_storage_sqlite
    PUBLIC  include
    PRIVATE src)

# uapp::core_search is PUBLIC for the same reason the other two are: the
# factory in include/uapp/storage/sqlite/sqlite_search_index.h hands back a
# std::unique_ptr<search::SearchIndex>, so every consumer of this target needs
# that header on its include path. uapp::core_database is PUBLIC on the same
# grounds: sqlite_database_store.h returns a
# std::unique_ptr<database::DatabaseStore>.
#
# NOTE THE DIRECTION. This target depends on core/database; core/database does
# NOT depend on it (core/database/CMakeLists.txt names neither uapp::sqlite3
# nor this target). That asymmetry is the layering rule, and it is checked by
# `nm -u libuapp_core_database.a` showing no sqlite3_* import rather than by
# review.
target_link_libraries(uapp_storage_sqlite
    PUBLIC  uapp::core_notes uapp::core_blocks uapp::core_search uapp::core_database
    PRIVATE uapp::sqlite3 uapp::compiler_options)
