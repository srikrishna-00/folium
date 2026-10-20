# `third_party/httplib` and `third_party/nlohmann_json` — a third vendoring
shape, not a repeat of pdfium's or libsodium's

This project already has two vendoring strategies, and this is deliberately
neither of them:

- `third_party/pdfium`: a **prebuilt binary**, gitignored, fetched by script,
  pinned by SHA-256 — because provenance of *compiled bytes* matters for the
  project's highest-risk parser, and a multi-platform binary matrix is tens
  of MB that git should not store forever.
- `third_party/libsodium`: a **system package**, found via pkg-config/
  `find_library` at configure time, version pinned but never vendored —
  because it is universally packaged and its behaviour is checked against
  external published test vectors, so a drifted version is a red test, not a
  silent risk.

Neither fits a single-header, pure-C++-source library:

- It is source text, not compiled output, so there is no "did we build this
  ourselves" question the way there is for PDFium.
- It is a few hundred KB to under 1 MB of text — small enough to commit
  directly without an LFS problem, and small enough that reading the diff of
  a version bump in one commit is a real, practical review step (unlike a
  7 MB `.dylib`).
- It is not reliably available as a single pinned file through a system
  package manager the way libsodium is — `apt`/`brew` packages for these
  split across many files and expect you to link a build, not drop in one
  header.

So the strategy here is the simplest one available for this specific shape
of dependency: **commit the header text directly**, pinned by upstream tag
and SHA-256 recorded in each directory's `VERSION` file, auditable by reading
the diff in the commit that bumps it.

## What is here

| Path | Committed? | What it is |
|---|---|---|
| `httplib.h` / `nlohmann_json/json.hpp` | yes | The vendored header text, verbatim from the pinned release. |
| `VERSION` | yes | Tag, exact fetch URL, SHA-256, licence. The audit trail. |
| `LICENSE` / `LICENSE.MIT` | yes | Licence text fetched from the same tag. |

## Why the fetch is not a CMake `FetchContent` / `file(DOWNLOAD)`

Same rule as `third_party/pdfium/README.md` states and `tests/support/
test_framework.h` relies on: `cmake -S . -B build` must work offline. CMake
only ever looks at these committed files; the network step (re-pinning to a
newer release) is one explicit command a human runs once, never part of
configure.

## What uses these

- `cmake/UappHttp.cmake` defines `uapp::httplib` (header-only `INTERFACE`
  target) from `third_party/httplib`.
- `cmake/UappJson.cmake` defines `uapp::json` (header-only `INTERFACE`
  target) from `third_party/nlohmann_json`.
- `api/` (the `uapp-server` executable) links both, for its HTTP server role
  and for JSON request/response bodies.
- `ai/remote/` links `uapp::httplib` for its HTTPS client role (calling the
  Anthropic API) — one HTTP library serving both the server and the client
  side of this project, by design, rather than vendoring two different
  stacks for the two roles.
