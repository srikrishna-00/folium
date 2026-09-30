add_library(uapp_file_detection STATIC
    src/byte_span.cpp
    src/capabilities.cpp
    src/checksum.cpp
    src/detector.cpp
    src/detector_path.cpp
    src/file.cpp
    src/format.cpp
    src/text_probe.cpp
    src/zip_container.cpp)
add_library(uapp::file_detection ALIAS uapp_file_detection)

target_include_directories(uapp_file_detection PUBLIC include)
target_link_libraries(uapp_file_detection
    PUBLIC  uapp::core_common
    PRIVATE uapp::compiler_options)

# ---------------------------------------------------------------------------
# What this target deliberately does NOT link, and why each absence matters.
#
#   uapp::sqlite3        — same guard rail as core/notes and core/blocks
#                          (PHASE0_SUMMARY.md H-3). Detection is pure: bytes
#                          in, a verdict out. It must never persist anything.
#   uapp::core_notes /
#   uapp::core_blocks    — the file engine is a peer engine, not a consumer
#                          of the document model. architecture.md section 4:
#                          "engines depend on PORTS, never on each other's
#                          internals."
#   libzip / minizip-ng  — file-format-support.md §7.1 gives both an ADOPT
#                          verdict, and the eventual ZIP *extraction* path
#                          will use one. Detection must not: it needs to read
#                          a handful of fixed-size header fields, and pulling
#                          a full archive library into the module that runs
#                          FIRST on every untrusted upload would hand that
#                          library the initial attack surface for no benefit.
#                          The central-directory walk in zip_container.cpp is
#                          ~200 lines of bounds-checked field reads and
#                          inflates nothing.
#
# The result is a library whose only dependency is uapp::core_common (for
# Status and ObjectId) and the C++17 standard library.
# ---------------------------------------------------------------------------
