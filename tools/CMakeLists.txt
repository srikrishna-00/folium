# tools/ — developer-facing executables. Not a product surface: the real
# entry points are the C ABI (libuapp_capi) and the platform apps. Kept out of
# core/ deliberately so nothing in the domain layer can depend on a CLI.
add_subdirectory(notes_cli)
