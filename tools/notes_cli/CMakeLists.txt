add_executable(uapp_notes_cli main.cpp)

# core_notes arrives transitively via storage_sqlite's PUBLIC link.
target_link_libraries(uapp_notes_cli
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

set_target_properties(uapp_notes_cli PROPERTIES OUTPUT_NAME "uapp-notes")
