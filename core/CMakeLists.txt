# core/ — the portable domain layer. Knows nothing about SQLite, HTTP,
# platform APIs or UI. Depends only on ports (abstract interfaces).
add_subdirectory(common)
add_subdirectory(notes)

# core/blocks — the Notion-like document model (spec section 14). Supersedes
# the flat note; both are built while the migration in ADR-0003 is pending.
add_subdirectory(blocks)

# core/capi is deliberately NOT added here. It is the composition root for the
# slice and therefore links the SQLite adapter, so it has to be configured
# after storage/. The top-level CMakeLists.txt adds it there, with a comment.
# Adding it here would also weaken the sentence above: capi is not part of the
# portable domain layer, it is the boundary wrapped around it.
