add_library(uapp_storage_sqlite STATIC
    src/fts_query.cpp
    src/search_index_sql.cpp
    src/sqlite_block_store.cpp
    src/sqlite_note_store.cpp
    src/sqlite_search_index.cpp)
add_library(uapp::storage_sqlite ALIAS uapp_storage_sqlite)

target_include_directories(uapp_storage_sqlite
    PUBLIC  include
    PRIVATE src)

# uapp::core_search is PUBLIC for the same reason the other two are: the
# factory in include/uapp/storage/sqlite/sqlite_search_index.h hands back a
# std::unique_ptr<search::SearchIndex>, so every consumer of this target needs
# that header on its include path.
target_link_libraries(uapp_storage_sqlite
    PUBLIC  uapp::core_notes uapp::core_blocks uapp::core_search
    PRIVATE uapp::sqlite3 uapp::compiler_options)
