# core/ — the portable domain layer. Knows nothing about SQLite, HTTP,
# platform APIs or UI. Depends only on ports (abstract interfaces).
add_subdirectory(common)
add_subdirectory(notes)

# core/blocks — the Notion-like document model (spec section 14). Supersedes
# the flat note; both are built while the migration in ADR-0003 is pending.
add_subdirectory(blocks)

# core/search — the unified search port (spec section 16). Added after notes
# and blocks but deliberately depending on NEITHER: a hit is described by a
# source-kind tag plus the shared 16-byte ObjectId, so the search layer does
# not link the domains it searches. The link line in core/search/CMakeLists.txt
# is what enforces that rather than this comment.
add_subdirectory(search)

# core/conversion — the first concrete edge of spec section 10's conversion
# engine: blocks::Page <-> Markdown, both directions, and ONLY Markdown. It
# belongs in the portable domain layer because that is exactly what it is: a
# pure in-memory transformation between the document model and a text format,
# with no I/O and no platform API.
#
# Unlike core/search it DOES link a domain — core/blocks, PUBLIC-ly — because
# the intermediate representation *is* blocks::Page rather than a second,
# parallel document model (see core/conversion/include/uapp/conversion/
# markdown.h, which records that decision and the correction it makes to
# docs/conversion-architecture.md section 2.1). It does NOT link
# storage/sqlite; the link line in core/conversion/CMakeLists.txt is what
# enforces that rather than this comment.
add_subdirectory(conversion)

# core/capi is deliberately NOT added here. It is the composition root for the
# slice and therefore links the SQLite adapter, so it has to be configured
# after storage/. The top-level CMakeLists.txt adds it there, with a comment.
# Adding it here would also weaken the sentence above: capi is not part of the
# portable domain layer, it is the boundary wrapped around it.
