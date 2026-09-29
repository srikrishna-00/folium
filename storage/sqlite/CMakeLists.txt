add_library(uapp_storage_sqlite STATIC
    src/fts_query.cpp
    src/sqlite_note_store.cpp)
add_library(uapp::storage_sqlite ALIAS uapp_storage_sqlite)

target_include_directories(uapp_storage_sqlite
    PUBLIC  include
    PRIVATE src)

target_link_libraries(uapp_storage_sqlite
    PUBLIC  uapp::core_notes
    PRIVATE uapp::sqlite3 uapp::compiler_options)
