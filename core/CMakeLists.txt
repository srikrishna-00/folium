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

# core/database — the property-typed database/collection system of spec
# section 15: a named schema of typed property DEFINITIONS plus rows of
# property VALUES, with a Table view (filter + sort + saved configuration)
# over them. It belongs in the portable domain layer because the parts that
# live here are pure: what a property TYPE is, what makes a row valid against
# a schema, and what a Table view MEANS (database::evaluate_table_view is the
# semantic definition the SQL compiler in storage/sqlite must agree with).
#
# Like core/conversion it DOES link a domain — core/blocks, PUBLIC-ly —
# because a Database is anchored to a blocks::Page rather than being a kind of
# page. That fork is argued in docs/adr/0004-database-anchored-to-page.md.
# It does NOT link storage/sqlite; the link line in
# core/database/CMakeLists.txt is what enforces that rather than this comment.
add_subdirectory(database)

# core/capi is deliberately NOT added here. It is the composition root for the
# slice and therefore links the SQLite adapter, so it has to be configured
# after storage/. The top-level CMakeLists.txt adds it there, with a comment.
# Adding it here would also weaken the sentence above: capi is not part of the
# portable domain layer, it is the boundary wrapped around it.
