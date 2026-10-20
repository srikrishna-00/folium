# Contributing & Development Workflow

**Status:** Phase 0 draft. Section 2 (Setup) was corrected 2026-10-20 to match the
real build, which now exists; everything from section 3 onward describes the
process this project is growing into as it gains contributors, not a claim
about tooling that exists today — treat it as direction, not as documentation
of a CI/PR pipeline that is already running.
**Version:** 0.0.0
**Date:** 2026-09-29

---

## 1. Read first

| Before you touch | Read |
|---|---|
| Anything | `docs/MASTER_PROMPT.md`, `docs/architecture.md` |
| `core/` | `docs/technical-requirements.md`, `docs/adr/0001-core-language-c-vs-cpp.md` |
| `storage/` | `docs/database-design.md` |
| A parser | `docs/file-format-support.md`, `docs/testing-strategy.md` §6 |
| `core/sync/` | `docs/sync-architecture.md` |
| `ai/` | `docs/ai-architecture.md` |
| `security/` | `docs/security-model.md` |
| A new dependency | `docs/file-format-support.md` §7 — **no exceptions** |

## 2. Setup

The real build, as it exists today — no bootstrap script or CMake presets exist yet, so use
plain CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build
```

A clean checkout with none of the optional dependencies installed still configures, builds, and
passes its full test suite. Three modules are optional and auto-detected (`AUTO`/`ON`/`OFF` via
`-DUAPP_ENABLE_PDF`, `-DUAPP_ENABLE_CRYPTO`, `-DUAPP_ENABLE_OCR`) because each needs a real
third-party dependency a fresh clone may not have:

```bash
brew install libsodium tesseract        # macOS; or apt install libsodium-dev libtesseract-dev
third_party/pdfium/fetch_pdfium.sh      # PDF viewing (prebuilt bootstrap, see its own README)
```

To also build under AddressSanitizer + UBSan (the sanitizer configuration every module in this
project has been verified against before being committed):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DUAPP_ENABLE_SANITIZERS=ON
cmake --build build-asan -j && ctest --test-dir build-asan
```

Target: well under 10 minutes on a clean machine for the default build. No Android/iOS build
scripts exist yet — `core/capi`'s C99 ABI is what a future mobile binding would target, but no
JNI or Swift layer has been written.

## 3. Workflow

1. **Issue first.** Every change traces to an issue with a phase label and acceptance criteria.
2. **Branch:** `type/short-description` — `feat/`, `fix/`, `perf/`, `refactor/`, `docs/`,
   `test/`, `chore/`.
3. **Follow the loop in spec §37** — the short version: understand the existing architecture,
   make the smallest coherent change, compile, test, fix, regression-test, update docs.
4. **Commit format:** Conventional Commits with a module scope.
   ```
   feat(pdf-engine): add AES-256 encryption via QPDF

   Implements P-08 for revision 6. Falls back to AESV2 for
   readers that cannot handle R6, controlled by an explicit option.

   Refs: #142
   ```
5. **PR:** fill the template, keep it reviewable (< ~400 lines of substantive change where
   possible), and link the issue.
6. **Review:** one approval for routine changes; **two, one of whom is a module owner, for
   `security/`, `core/sync/`, `storage/` schema, the C ABI, and any new dependency.**
7. **Merge:** squash to `main`. `main` is always releasable.

## 4. The rules that are not negotiable

From spec §37 and §38, restated as review criteria:

- **Never delete or disable a test to make a build pass.** Quarantine with an issue and an
  owner, or fix it.
- **Never suppress a warning without a comment justifying it.** `-Werror` is on; a suppression
  is a documented decision.
- **Never disable a security check to make a feature work.**
- **Never rewrite a working module without a stated reason** in the issue.
- **Never claim a feature works that is not implemented and tested** (spec §5). If it is not
  done, it is marked `NOT IMPLEMENTED` in the docs, in that wording.
- **Never add a dependency without completing the checklist in §7.**

## 5. Code standards

### C/C++ (core)
- C++17 internals, C99 at the ABI. `-fno-exceptions -fno-rtti`.
- Errors are returned, never thrown. All fallible returns are `[[nodiscard]]`.
- RAII for every resource. No raw owning pointers. `unique_ptr` by default; `shared_ptr` only
  where sharing is genuinely required and documented.
- No unsafe string functions (`strcpy`, `sprintf`, `strcat`, `gets`). Use the bounded wrappers
  in `core/util/`.
- Check every allocation. Check every integer operation that can overflow, using the checked
  helpers.
- Validate all external input at the boundary. Parsers assume nothing.
- Document thread affinity for every public type. "Thread-safe" is a claim that needs a test.
- `clang-format` (config in-repo) and `clang-tidy` run in the pre-commit hook.

### Kotlin / Swift
- Follow the platform style guides. `ktlint` / `swiftlint` enforced.
- No business logic above the binding layer. If a rule is being implemented in Kotlin *and*
  Swift, it belongs in the core.
- Every native handle wrapper is closeable/deinit-ing and tested for leaks.

### Go (server)
- Standard Go style; `golangci-lint`. Errors wrapped with context. Structured logging only.

## 6. Testing obligations

Detail in `docs/testing-strategy.md`. Minimum bar for any PR:

- [ ] Unit tests for new logic, **including every error path**
- [ ] A fuzz target + seed corpus for any new parser (**CI enforces this**)
- [ ] Integration tests for new pipelines
- [ ] A benchmark for anything with a budget in `performance-strategy.md`
- [ ] Suite passes under ASan+UBSan
- [ ] Coverage gates met (80 % general; 90 % for sync, crypto, permissions, detection)

## 7. Adding a dependency — the checklist

**No dependency is added without all nine items.** CI enforces 4, 5 and 8.

1. Is it actually needed? Can the core do it in comparable effort with less risk?
2. Licence identified by SPDX id, with upstream text vendored at `third_party/<name>/LICENSE`.
3. **Verdict recorded in `file-format-support.md` §7.1.** GPL/AGPL/LGPLv3 are **hard blocks**
   for the client (§7.2) — do not open the PR.
4. Pinned by commit hash **and** SHA-256.
5. Row in `THIRD_PARTY_NOTICES` (generated by CI, but the metadata must be present).
6. Static-linking and iOS-distribution compatibility confirmed.
7. Named owner responsible for tracking its CVEs.
8. `METADATA.yaml` with name, version, SPDX id, upstream URL, owner, patches.
9. Binary-size impact measured and stated in the PR.

If the licence is dual or weak-copyleft, state explicitly which licence we elect and why.

## 8. Architecture Decision Records

Write an ADR when a decision is **expensive to reverse**: a language or framework choice, a
schema or protocol shape, a security mechanism, a major dependency, or anything that changes
a documented invariant.

```
docs/adr/NNNN-short-title.md
```
Format: Status · Context · Decision · Alternatives Considered · Consequences · Revisit Criteria.
Superseded ADRs are marked, never deleted — the reasoning stays visible.

## 9. Documentation

Docs are part of the change, not a follow-up:

- Public API change → update `api-design.md` and `api/openapi.v1.yaml`
- Schema change → `database-design.md` + a migration + a fixture DB at the previous version
- **New format capability → `file-format-support.md` AND a fixture proving it**
  (the matrix verification test will otherwise fail the build — `testing-strategy.md` §14)
- New threat or mitigation → `security-model.md`
- Phase completion → `roadmap.md` + `changelog.md`

## 10. Definition of Done (spec §41)

A change is done when:

```
[ ] Implemented
[ ] Compiles clean, no new warnings
[ ] Unit tested
[ ] Integration tested where applicable
[ ] Error handling implemented (codes, not strings)
[ ] Security reviewed
[ ] Performance considered (benchmarked if it has a budget)
[ ] Documentation updated
[ ] No known critical defects
[ ] Regression tests pass
```

"Works on my machine" and "I'll add tests in a follow-up" do not satisfy this.

## 11. Reporting a security issue

Do **not** open a public issue. See [`SECURITY.md`](../SECURITY.md) — use GitHub's private
vulnerability reporting. Include reproduction steps and affected versions.

## 12. Communication norms

- Disagree in the PR, not after the merge.
- "I don't understand this" is always a valid review comment and usually a sign the code needs
  a comment.
- Uncertainty is stated, not hidden. `TBD` and `NOT IMPLEMENTED` are honest and encouraged;
  a confident claim that turns out to be false is the thing we are trying to avoid everywhere
  in this project.
