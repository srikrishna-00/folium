# Changelog

All notable changes to this project are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Per spec §5, nothing is recorded here as complete unless it is implemented and tested.
Planning and architecture work is recorded as such.

---

## [Unreleased]

### Added — `security/crypto`: cryptographic primitives on libsodium (spec §23/§38, ADR-0002 decision 1)

The foundation `adr/0002` decision 1 ("E2EE from v1.0") stands on, and **only** the
foundation. `architecture.md` §4's layer diagram already named this module —
"`security/crypto (libsodium)`", in the ADAPTERS row — so it went where the architecture put
it rather than into `core/`: `core/` is the portable domain layer that "knows nothing about
SQLite, HTTP, platform APIs or UI", and a wrapper around a specific third-party C library is
an adapter by the same rule that puts `storage/sqlite` outside `core/`.

**Dependency vetted and pinned first — `docs/spikes/SP-9-findings.md`.** This is a new spike,
not one of `technical-requirements.md` §6's SP-1…SP-8 (that table predates the E2EE decision
and has no crypto-dependency row). It follows the SP-1 precedent: licence verified against the
installed bytes rather than upstream's website, a `third_party/libsodium/VERSION` pin recording
version + upstream tarball SHA-256 + the dev-host artefacts, `LICENSE`/`AUTHORS` copied out of
the prefix, a documented re-roll procedure, and a `THIRD_PARTY_NOTICES` SECTION 0 entry.
**libsodium 1.0.22, ISC.** SECTION 1 had listed libsodium as approved since Phase 0 on the
strength of upstream documentation; that row is now confirmed against the bytes, which is the
difference between SECTION 1 and SECTION 0.

**The licence is a non-event and the finding is somewhere else.** ISC imposes nothing beyond
attribution and — unlike FreeType's FTL or anything LGPL — makes **no static-vs-dynamic
distinction**, so static linking into a closed-source App Store binary is unambiguously
permitted. What the vetting actually turned up is that bundling a crypto library providing
*content confidentiality* makes the app's encryption **non-exempt** for Apple's purposes,
pulling in an **EAR Category 5 Part 2 self-classification report to BIS/NSA before export and
annually thereafter**, plus a France declaration. That follows from ADR-0002 decision 1 rather
than from libsodium, it is a legal/release-process deliverable rather than an engineering one,
and nothing in the repo owned it: **new risk R-33**, plus a new gate row in
`security-model.md` §13. Reviewing a crypto dependency's licence and concluding "clear to
ship" is the wrong conclusion from the right evidence.

**Implemented:** X25519 device keypair generation (`crypto_kx_keypair`); CEK and Argon2id salt
generation; XChaCha20-Poly1305-IETF `seal`/`open` with associated data; authenticated CEK
wrapping/unwrapping between two device keypairs; Argon2id passphrase derivation with three
named cost profiles; and `SecureBytes`, a `sodium_malloc`-backed move-only buffer used for
every secret **and for recovered plaintext**.

**Rule 0, enforced by the build graph rather than by review:** *we do not implement
cryptographic primitives* (spec §38). Everything delegates to libsodium — verifiable as 15
imported symbols and no arithmetic over key material. `uapp::sodium` is linked **PRIVATE**, and
`crypto.h` declares its own `kNonceBytes`/`kTagBytes`/… `static_assert`ed against libsodium's
macros in the `.cpp` precisely so the public header never needs `<sodium.h>` — which makes
PRIVATE enforceable rather than aspirational. Exactly two files in the tree include the
libsodium header, and `nm -u` shows zero libsodium imports in all seven other first-party
libraries.

**Nonce strategy, decided and documented rather than defaulted into.** Nonce reuse under
ChaCha20-Poly1305 is catastrophic (repeated keystream *and* a leaked Poly1305 one-time key,
i.e. arbitrary forgery), and it is the most common way real AEAD deployments break. **A fresh
random 192-bit nonce is generated inside `seal()` from the platform CSPRNG, and the API gives
the caller no way to supply, influence or reuse one — there is no `seal_with_nonce()` overload,
not even a test-only one.** That absence is the control: an API accepting a nonce cannot
prevent its misuse by documentation, and a test-only back door is a production back door with
a comment on it. Safe because the nonce is 192 bits: collision probability is `< q²/2^193`,
i.e. `2^-129` at `2^32` encryptions under one key and `2^-65` at an absurd `2^64` — against
AES-GCM's `~2^-33` at `2^32` with a 96-bit IV, which is why NIST SP 800-38D caps a random-IV
GCM key at `2^32` invocations. **The primitive choice and the nonce strategy are one decision,
not two.** `security-model.md` §5's *deterministic* chunked-blob nonce
(`HKDF(file_key, chunk_index)`) remains the right design for that path, is NOT IMPLEMENTED,
and must arrive as a separately-named entry point whose caller owns the index.

**Key wrapping diverges from a written design, deliberately, and the divergence is flagged
rather than absorbed.** `security-model.md` §5A.2 names `crypto_box_seal`, which uses an
ephemeral sender keypair and therefore authenticates **nobody**. Given T-305 ("a
malicious/compromised device pushes poisoned sync data"), the implementation uses the
**authenticated** variant: `crypto_kx` (X25519 + BLAKE2b over both static public keys)
deriving a *directional* KEK, then the module's own XChaCha20-Poly1305 over the CEK with both
public keys bound into the AAD. The recipient authenticates the sender, a wrapped key cannot be
replayed back at its sender, and the module keeps **one** AEAD rather than two (`crypto_box`
uses XSalsa20-Poly1305). It gives up sender anonymity. §5A.2 does not say which property it
wants; that ambiguity is annotated there as an open question for a human, with the note that if
both are needed the anonymous form is an *additional* function, never a replacement.

**Argon2id parameters conflict with `security-model.md` §5, and §5 is wrong.** §5 specified
`opslimit=4, memlimit=256 MiB` "where we can afford it" — written before `adr/0002` decision 3
fixed the floor at 2 GB RAM / iOS 15+ / Android 8+, on which a 256 MiB transient allocation is
a foreground low-memory kill. The implementation uses **`opslimit=3, memlimit=64 MiB`**, which
is libsodium's *own documented procedure* applied to our constraint ("set `memlimit` to the
amount of memory you want to reserve… then set `opslimit` to 3") with 64 MiB being the largest
of its three presets that fits. Measured 61 ms on an Apple M4 (vs 42 ms for the INTERACTIVE
preset's `opslimit=2`, 277 ms for MODERATE's 256 MiB, 1563 ms for SENSITIVE's 1 GiB);
estimated 0.4-0.6 s on floor hardware. **The cost is stated rather than minimised:** a 4x cut
in `memlimit` is a 4x cut in an offline attacker's memory cost per guess. Accepted because the
alternative is a passphrase path that cannot run on the committed device floor, and because
§5A.4 roots recovery in a generated 24-word phrase (~256 bits) rather than a user-chosen
passphrase. §5 is annotated in place. **Measuring on real floor-tier hardware is owed** — the
same class of gap as SP-3/SP-4's Tier D re-runs. `crypto_pwhash_ALG_ARGON2ID13` is passed
explicitly, never `ALG_DEFAULT`, which changed meaning (Argon2i → Argon2id) at libsodium
1.0.15: a derived key whose algorithm depends on the linked version is a key that silently
stops deriving after a dependency roll.

**Secure memory, with its limits stated.** `SecureBytes` wraps `sodium_malloc` for guard pages,
a canary, `mlock` (no swap), and zero-on-free; it is move-only so a key cannot be duplicated
into a second unmanaged lifetime by an implicit copy. `open()` writes recovered plaintext into
a `SecureBytes` rather than a `std::vector`, because decrypted document content *is* asset A1
and a `std::vector`'s buffer is reclaimed without zeroing. Ciphertext, public keys, nonces and
salts stay in ordinary `std::vector`/`std::array` — they are not secret, and guarded pages are
a scarce resource. **What it is not:** it is not a Secure Enclave. `security-model.md` §5A says
device keys are generated in hardware and never leave it; these are generated in ordinary
process memory with better hygiene. `security/key-management` does not exist. Also recorded:
`sodium_mlock` can fail silently on a low `RLIMIT_MEMLOCK` and libsodium exposes no way to ask
whether it did.

**Tests — 27 cases, 7 ctest entries, built on published vectors rather than round trips.** A
round trip passes for *any* pair of mutually-inverse functions, including a wrapper that
silently drops the AAD or confuses XChaCha20 with the original ChaCha20 — both invisible to a
round trip and catastrophic against a real attacker. So: `draft-irtf-cfrg-xchacha-03` §A.3.1's
AEAD vector is driven through the real `open()` (the decrypt direction on purpose: `seal()` has
no nonce parameter and adding a test-only one would be adding the back door the nonce strategy
forbids), with one-bit mutations in **both** the ciphertext and the tag region, and
wrong-AAD/no-AAD rejection; RFC 7748 §6.1's Alice/Bob keypairs are used as real device keys
through the real wrap/unwrap path in both directions, plus small-order public-key rejection.
**No external Argon2id KAT is possible** and this is structural, not an omission: RFC 9106's
vectors use parallelism `p=4` and libsodium hardcodes `p=1`, so golden regression vectors are
used and labelled as such — what they catch is a dependency roll silently changing the
derivation, which would lock every existing user out of their own data. Failure modes are
exhaustive: wrong key, substituted nonce (a *genuine* nonce from another ciphertext under the
same key, which is the realistic attack), every truncation length from 0 to full, wrong AAD,
third-party unwrap, misattributed sender, every-7th-byte tampering — each asserting the
recovered buffer is **empty**, because "no plaintext on authentication failure" is a security
property and is tested as one. All five causes return the **same** `CryptoAuthFailed`: anything
finer is a decryption oracle. Plus 20 000-sample nonce and 2 000-sample key uniqueness
sampling, with the test stating plainly what it *cannot* show (it cannot confirm the birthday
bound; it catches a constant, a counter, a clock or an unseeded PRNG, which are how this
actually breaks).

**Build integration.** `UAPP_ENABLE_CRYPTO` is the same tristate as `UAPP_ENABLE_PDF` — AUTO
detects, ON fails configure with the install command in the message, OFF skips — because a
second dependency switch behaving differently from the first would be a trap. What differs is
acquisition: `cmake/UappSodium.cmake` finds libsodium on **system** paths (pkg-config, then
`find_library`, then the version read out of `sodium/version.h`), **inverting**
`UappPdfium.cmake`'s `NO_DEFAULT_PATH` rule. Argued in `third_party/libsodium/README.md`: that
rule protects against linking an unpinned *parser* of unknown vintage, which is worth much less
for a library whose entire observable behaviour is pinned by two external published test
vectors and a runtime version assertion — a surprise libsodium is a red test, not a silent
behaviour change. Unchanged from the PDFium precedent: **CMake never touches the network**.
Verified: AUTO → ON (46/46 tests), OFF → 39/46 with everything else unaffected, ON with nothing
findable → hard configure error. Clean under Debug and under Debug+ASan/UBSan with
`-Werror -fno-exceptions -fno-rtti`.

**NOT IMPLEMENTED** (listed in full in `crypto.h` at the point a reader would otherwise assume
otherwise): the key hierarchy (Recovery Secret → URK → WSK → document key → per-file key,
§5A.1) and therefore no `crypto_kdf_derive_from_key` wrapper; chunked blob encryption (§5);
device enrolment, the out-of-band short-code check, WSK rotation and revocation (§5A.2, §5A.3);
**Ed25519 device identity keys and mutation signing** (§4, §5 — this module does key *agreement*
only, and conflating agreement with signing in one keypair is the start of using one key for
two purposes); hardware-backed key storage; post-quantum anything (§5A.6 pins "X25519 only"); a
fuzz target over `open()`; and **a C ABI**. The ABI is deferred on a stronger argument than the
other modules': the shape of these functions will change once the hierarchy above them exists,
and an ABI is a promise. The nine new `Status` codes are therefore flattened to
`UAPP_STATUS_UNEXPECTED` in `core/capi` following the precedent set five times already — with a
note that when they *are* minted, `CryptoAuthFailed` must stay **one** constant, because
splitting it would freeze a decryption oracle into a stable interface.

### Added — `core/database`: property-typed databases and Table views (spec §15)

First real slice of spec §15's Database System: a named collection of typed property
*definitions* (the schema) plus *rows* of typed property *values* conforming to it, anchored
to a host Page as a sibling of Blocks (spec §14's own hierarchy line: `Page -> Blocks / Child
Pages / Databases / Files`).

**Architectural decision — `docs/adr/0004-database-anchored-to-page.md`:** a Database is its
own entity with its own `DatabaseId`, referencing a host `blocks::PageId`, rather than (a) a
kind of Page (`page.kind = database`, what `database-design.md` §3.4 originally specified) or
(b) a block payload (`BlockType::Database`). (a) was rejected because `blocks::Page` is a
narrow, concrete type with four real readers — `BlockStore`, `DocumentService`, the Markdown
converter, and the already-frozen C ABI — none of which can safely be taught "some pages
aren't documents" without a schema migration touching a published ABI. (b) was rejected
because a schema plus option sets is orders of magnitude larger than a block's content, and it
would make one database's lifetime equal to one block's, making multiple views of one
collection on different pages impossible. The chosen shape is purely additive: new tables,
zero edits to `page`, `block`, `core/conversion`, or `core/capi`. Recorded as a deliberate
deviation from `database-design.md` §3.4 (annotated in place), deferred rather than rejected:
once child pages and `page.kind` land, a "full-page database" becomes exactly what §3.4
originally specified.

**Property types implemented** (a closed tagged union, not a stringly-typed bag — a Select
value actually references one of its property's defined options, not a free string that
happens to match): Text, Number, Boolean, Select, Multi-select, Date. **Declared, NOT
IMPLEMENTED** with machine-readable reasons (same `_not_implemented_reason()` precedent
`core/blocks` set): Person, URL, Email, File, Relation, Rollup, Formula — Relation/Rollup need
cross-database references and Formula needs an expression evaluator, both real future
features, not oversights.

**Views:** Table only — filtering (equality/comparison operators appropriate to each property
type), sorting (ascending/descending, any property), and named saved-view configurations
(filter+sort, persisted separately from row data). Board/Calendar/Timeline/Gallery/List
declared, NOT IMPLEMENTED with reasons: every one of them is Table plus a projection (a Board
needs a Select to group by, a Calendar/Timeline needs a Date, a Gallery needs a File/image), so
Table had to land first. `database::evaluate_table_view` (an in-memory specification) and
`query_table_view` (the SQL the adapter actually runs) are tested **differentially** — both
must produce the identical row sequence over the same data, so the SQL is checked against a
specification rather than against a reading of it.

Limits, each justified rather than picked for looks (same discipline as
`blocks::kMaxBlockDepth`): `kMaxPropertiesPerDatabase` = 100, `kMaxRowsPerDatabase` = 100,000,
`kMaxSelectOptionsPerProperty` = 256, `kMaxPropertyTextBytes` = 4096 (distinct from
`blocks::kMaxBlockTextBytes`'s 1 MiB — a block is document content, a property value is a
spreadsheet cell), `kMaxFiltersPerView` = 32, `kMaxSortsPerView` = 8, `kMaxViewsPerDatabase` =
64, `kMaxDatabaseNameBytes` = 512 (matches `blocks::kMaxPageTitleBytes` exactly, since a
database's name is shown in the same UI surfaces a page title is).

Persistence in `storage/sqlite/src/sqlite_database_store.cpp`: real relational tables
(`db_collection`, `db_property`, `db_select_option`, `db_row`, `db_value`, `db_value_option`,
`db_view`, `db_view_filter`, `db_view_sort`), not a stringified blob — values keep their type on
disk and are efficiently filterable/sortable by the Table view. Validation runs on write AND on
read (spec §28: a synced or restored `.db` file is untrusted input even though it used to be
ours) and is total against the schema: every property id a row carries must be one the schema
defines, every value must match its property's declared type, every Select/Multi-select choice
must be one the property's own option set defines.

**Tests: 68 real assertions**, including a bug this project's own test suite caught in itself,
not the product: a test building two independent databases on the same host page reused a
fixture property's hardcoded id (`date_property()`'s fixed `PropertyId`) across both, colliding
on `db_property.id`'s global primary key — a scenario a real `IdGenerator` would never produce,
since every id in this codebase is globally unique by construction. Fixed in the test fixture,
not the schema. Verified independently (not just agent self-report): a full project rebuild at
`-Werror` with zero warnings, `ctest` 39/39 (was 29/29, +5 new suites, after the fixture fix),
the new suite's own binary confirming 68/68 directly, a full from-scratch Debug+ASan/UBSan
rebuild of the entire project (not just the new module) also 39/39 clean, and `nm -u` on the
built `core/database` static library confirming it reaches `core/blocks` and `core/common`
only — never `storage/sqlite` directly, matching every other core module's layering.

NOT IMPLEMENTED, each with its own documented reason in `core/database/include/uapp/database/
*.h`: rows-open-as-pages (would entangle a row's lifetime with a page's, and needs to write to
the `page` table this module does not own), schema evolution / property retyping after creation
(a product decision about data loss under sync, not a coding gap), manual row reordering (needs
the fractional-index work already deferred for blocks), workspace-scoping (inherits the host
page's implicit workspace, same as `page` itself), and no C ABI surface yet (16 new `Status`
codes flatten to `UAPP_STATUS_UNEXPECTED`, consistent with how every prior module landed before
getting ABI coverage).

### Added — `core/conversion`: `blocks::Page` ↔ Markdown, both directions (spec §10)

**The first concrete edge of the conversion engine, and a correction to the architecture it
implements.** `docs/conversion-architecture.md` §2.1 specified a `UDMDocument` type with
styles, numbering, sections, resources and a 15-alternative `BlockNode` union. **That type is
NOT IMPLEMENTED and is not what this converter translates through.** The Universal Document
Model in this codebase is `blocks::Page` + `blocks::Block`, which ADR-0003 already made the
project's one document representation; a second parallel model would make every conversion
*two* lossy hops (Markdown → UDM → Page) and would let the editor and the converter disagree
about what a document is. So the IR is the live domain model, and `UDMDocument` is re-scoped
to the **superset `blocks::Block` must grow into as formats land — a backlog, not a type**.
§2.1, §5.1 and §6.2 of that document are annotated to match, each gap naming its own
authority; `core/conversion/include/uapp/conversion/markdown.h` says the same thing from the
code side, and the two must not drift.

Markdown first and **only** Markdown, for reasons that are properties of the format rather
than preferences: it needs no third-party dependency and therefore no spec §35 licence
review; CommonMark's leaf blocks are almost exactly the eight implemented block types; and it
is useful with nothing else built (export a page to `.md`, import a `.md` as a page).

- **What round-trips EXACTLY (Page → MD → Page → MD is a fixed point, asserted
  byte-identically).** All eight block types with content, order and nesting: paragraph;
  heading levels 1–3; bulleted, numbered and checklist items; fenced code with its language
  tag; divider; quote. Numbered items are numbered **from sibling position**, never from the
  payload, because that is where `core/blocks` says numbering lives. Nesting under a list item
  works to any depth Markdown indentation allows and is **not limited to nested lists** — a
  paragraph, heading, code block, divider or quote nested under an item round-trips too,
  because the item's children are written at its content column and read back by recursively
  parsing that indented range as a sub-document. Multi-line text in a paragraph, quote or list
  item round-trips, **including an interior blank line**, and a text line that would itself
  parse as a different block (`- not a bullet`, `1. not ordered`) is backslash-escaped on the
  way out and unescaped on the way in.
- **One lexer, one escape, and that is the whole correctness argument.** `src/markdown_syntax.h`
  is shared by the writer and the reader so that the writer's "must I escape this line?" and
  the reader's "does this line end the paragraph?" cannot drift into silent corruption. The
  invariant `unescape_text_line(escape_text_line(t)) == t` holds for **every** input, not only
  for trimmed input, and is tested directly before anything is built on it: 53 hand-picked
  adversarial lines (one per rule, plus the backslash-heavy shapes that found bugs while it was
  written) and **95 803 exhaustively generated strings** — every string of length 0–4 over the
  14 characters the block classifiers react to, and every string of length 5–6 over the hot
  subset. Each one is checked for two properties, not one: invertibility, *and* that the
  escaped line is neither blank nor classifiable as a block start.
- **What is lossy is lossy because of a GAP IN THE BLOCK MODEL, not a shortcut here**, and the
  reasoning is not duplicated — each case names the `core/blocks` declaration that owns it.
  Inline `**bold**` / `[text](url)` / `![alt](src)` are kept **verbatim**, byte for byte
  (`preserved_note`/`inline_markup`): stripping the syntax to its plain text would destroy the
  href, which is the one part a future span model could not reconstruct. Tables and images
  become paragraph text (`approximated`/`table`, `/image`) — `BlockType::Table = 10` and
  `Image = 11` are declared and NOT IMPLEMENTED, so there is no variant alternative to put
  them in; the lines come back out unchanged, so a future Table type can re-parse them.
  Heading levels 4–6 are **clamped to 3** (`heading_level_clamped`), not dropped and not
  demoted to a paragraph — the one in-scope construct whose MD → Page → MD is deliberately not
  byte-identical, because losing a heading's heading-ness is worse than losing one level of it.
  Children of a non-list block are **flattened** to the parent's level in document order
  (`nesting_flattened`) — Markdown derives nesting from containers only, and refusing the
  export would make a legal page un-exportable. Leading/trailing whitespace in a text line is
  normalised and reported (`text_line_whitespace`). A code info string containing whitespace
  reads back as its first word (`code_language_truncated`); a backtick in the tag switches the
  fence to `~~~`, which **can** carry it, so that case is lossless.
- **Three read-side constructs are NOT IMPLEMENTED and are not silently mis-parsed.** `---`
  under a paragraph resolves as a **thematic break**, not a setext heading, because that is
  what this module's own exporter means by it — the alternative would re-import every exported
  paragraph-then-divider pair as a heading, i.e. break the round-trip the module exists for; a
  `===` run becomes paragraph text and is reported (`setext_heading`). An indented code block
  is paragraph text (`indented_code_block`), because indentation is reserved for list nesting.
  Link reference definitions, footnotes, front matter, HTML blocks and block equations are kept
  verbatim as paragraph text (`preserved_note`/`unmodelled_block`).
- **Nothing is silently dropped, and `lost_content()` is false by construction.** There is no
  input this converter discards without either returning a non-Ok `Status` or filing a
  `FidelityReport` entry. `Fidelity::Dropped` is declared — the report shape is shared with
  converters that will need it — and is **never emitted here**, which the suite asserts over a
  document containing every lossy construct at once. Entries aggregate by
  `(severity, construct)`, so a 40 000-line file with a table on every line produces one entry
  and not 40 000 unbounded allocations driven by untrusted input (spec §38).
- **Both directions are bounded, because both are driven by untrusted input** — a `.md` file on
  import, a synced or restored page on export. 16 MiB in, 16 MiB out, each with its own cap
  because a page at the block model's own limits would serialise to ~50 GB. Failure modes, all
  tested: `CONVERSION_INPUT_TOO_LARGE`, `CONVERSION_INPUT_MALFORMED` (an embedded NUL, which
  would truncate at the first C ABI boundary exactly as `validate_block_content` rejects one),
  `CONVERSION_OUTPUT_TOO_LARGE` with `out` left **empty rather than half-written**, plus the
  **existing** 5xx codes for everything that is really a block-model violation —
  `BLOCK_DEPTH_EXCEEDED` (list nesting past `kMaxBlockDepth`, checked on entry so a 4 000-level
  file costs 64 frames and a `Status`, not a stack), `BLOCK_TEXT_TOO_LARGE`,
  `PAGE_BLOCK_LIMIT_EXCEEDED`. Three new `uapp::Status` codes and **no new ABI constant**: see
  "Not implemented" below. Empty or whitespace-only input is `Ok` with zero blocks, because an
  empty document is a legal empty page and reporting a non-error as an error is its own lie.
- **Isolation is a property of the link graph, not of a comment.** `uapp::core_conversion`
  links `uapp::core_blocks` (PUBLIC — the IR *is* `blocks::Page`) and nothing else; the test
  binary links only `uapp::core_conversion`. `nm -u` on the built archive shows **zero**
  `sqlite3_*`, `uapp::notes`, `uapp_capi` or PDFium imports — its only first-party undefined
  symbols are four `core/blocks` validators plus `block_type_of`, `object_id_is_nil`, and its
  own internal lexer —
  and `otool -L` on the linked test binary shows only `libc++` and `libSystem`. A conversion is
  a pure in-memory transformation of a value the caller already holds; if it could reach a
  database handle it would need a transaction story it has no business having.
- **Results. 45 cases, 45 pass, in one new binary and five CTest entries** (CTest goes 24 → 29):
  13 on the lexer and the escape invariant (incl. the 95 803 generated inputs), 11 on
  round-tripping (eight block types, multi-line text, nested lists, non-list blocks under a
  list item, sibling-position numbering, a realistic 14-block mixed document, both halves of
  the title option, empty input, fence sizing, CRLF, nesting at exactly `kMaxBlockDepth`), 13
  on fidelity (two on the report's own aggregation, then **one per documented lossy construct**,
  each asserting the construct id, the severity *and* the resulting text — not merely that
  nothing crashed), and 8 on bounded failure. Clean under Debug and under Debug + ASan/UBSan
  (`-DUAPP_ENABLE_SANITIZERS=ON`), `-Werror -fno-exceptions -fno-rtti` throughout, zero
  warnings. One real bug was found and fixed by the suite: a list item's trailing blank lines
  were being recursed into as a child region, which cost a depth level for no block and failed
  a file nested exactly `kMaxBlockDepth` deep at its last item.
- **NOT IMPLEMENTED, and deliberately so.** **Every other format**: DOCX, PDF, HTML, EPUB, RTF,
  ODT, TXT and every other row of `conversion-architecture.md` §5. Each needs a real format
  parser, and the Office/EPUB rows need third-party libraries that do not exist in this
  repository and have not had the spec §35 licence review; none of them is faked, and §5.1 is
  annotated to say exactly one of its cells is real. There is also **no `ConversionPlanner`, no
  capability graph and no job protocol** — this is one edge of §4's graph, implemented, not the
  graph. And **no C ABI**: `uapp::Status` grew `ConversionInputTooLarge` / `…InputMalformed` /
  `…OutputTooLarge` (800–802), but `core/capi` mints **no** `UAPP_STATUS_CONVERSION_*`
  constant and flattens all three to `UAPP_STATUS_UNEXPECTED` — unobservably, since no C
  caller can reach a conversion today. That is the precedent `core/blocks`, `core/search`,
  `file-engine` and `pdf-engine` each landed on, applied a fourth time rather than reversed:
  the module stabilises first, the frozen boundary follows. When a `uapp_markdown.h` exists
  these become real constants in an 8xx block and that is an additive MINOR bump.

### Added — `core/sync/spike`: the Fugue CRDT correctness spike (ADR-0002, SP-6 R-1/R-2/R-3)

**Research code, not a product module, and the build graph says so.** `uapp::sync_fugue_spike`
links nothing but `uapp::compiler_options` — not `uapp::core_common`, not `storage/sqlite`, not
`core/capi` — so "the CRDT experiment cannot reach the document model" is checked by the linker
rather than by review, and `-DUAPP_BUILD_SYNC_SPIKE=OFF` removes it entirely (CTest drops from
24 entries back to the pre-existing 19). It operates on a sequence of `char`, deliberately: this
spike tests the *algorithm*, and integrating it into `core/blocks` before the algorithm was
proven is the mistake it exists to avoid.

Full findings, with numbers: **`docs/spikes/fugue-correctness-spike.md`**.

- **Fugue and FugueMax, both implemented from the paper.** Algorithm 1 of Weidner & Kleppmann
  (arXiv:2305.00583) line for line, with the paper's line numbers quoted at each step, plus
  Definition 6's FugueMax right-sibling rule. Both, because `adr/0002` claims "maximal
  non-interleaving" and that property belongs to FugueMax, not to Fugue.
- **Three independent oracles, not one.** A reference model that computes the same order by a
  structurally different route (whole operation set, precomputed root paths, O(n²) pairwise
  comparator, brute-force ranks that also *prove the comparator is a total order*); a
  strong-list-specification oracle derived from what each replica actually saw, which is
  independent of Fugue altogether; and a maximal-run detector for non-interleaving.
- **Classic RGA, implemented on purpose as a defective control,** calibrated against the Fugue
  paper's Appendix A.1.8 (which states the expected `axb` outcome). The suite asserts the
  detector **fires** on RGA, which is what turns P4 from a claim into a measurement.
- **Results.** 25 cases, 25 pass. Soak: **5 487 225 generated operations / ~27 M applications**
  across six randomised generators, clean; ASan+UBSan clean over 731 881 operations; 9.7 s on
  the PR path, 115 s for the soak, one binary, `UAPP_FUGUE_TRIALS` picks which. P4 concretely:
  the backward shopping-list shape merges to `SXaYbZc` under RGA (3 runs per token, every
  replica agreeing) and to `SabcXYZ` under Fugue (1 run per token).
- **Mutation-tested, because 25/25 on the first run is a reason for suspicion, not confidence.**
  Five faults injected one at a time into `fugue.cpp`, each rebuilt with the output binary's
  hash verified to have changed. All five are now caught; **one initially was not**, and that
  gap — the reference model is structurally blind to a bug in operation *generation*, and the
  generator under-produced tombstones — is the most useful thing in the findings doc. It
  produced a new required oracle (P5) in `sync-architecture.md` §6.2, two tombstone-heavy
  generators, and a raised default trial count.
- **Known gaps, stated rather than implied.** No tombstone GC, no run-length/waypoint
  compression, no persistence, no encryption, no transport — all Phase 13 production work with
  their own design obligations. The naive local insert is O(n) per operation, i.e. O(n²) for
  sequential typing (1.17 s for 16 000 appends), which is the cost the paper's own optimised
  implementation removes with waypoints.

**Consequences recorded elsewhere:** `risk-register.md` **R-29 downgraded M×5=10 → L×5=5** and
re-scoped to the algorithm choice, with **new R-32 (M×5=10)** for the untested optimised/GC'd
implementation the probability moved to; `roadmap.md` Phase 13 entry and exit criteria updated
(effort unchanged at 93 ew); `sync-architecture.md` §6.1/§6.2 and `adr/0002` annotated with two
corrections the spike found in *their own text*, flagged for product-owner sign-off rather than
edited silently. **Still owed and not touched here:** the encrypted framing sketch (re-scoped
SP-6 task 4) and the tombstone-GC / causal-stability specification (SP-6 R-4).

### Added — `core/capi`: file detection and PDF viewing C ABI (ABI **0.3**, spec §8/§9/§11, §30)

Closes the last mobile-bridge gap the 0.2 entry below left open in its own NOT IMPLEMENTED
list: `core/capi` now exposes every module that exists in the repository. `uapp_file.h` wraps
`file-engine/detection` and `uapp_pdf.h` wraps `pdf-engine/reader`; OCR/scanner/sync/AI remain
absent because those engines do not exist yet, not because the boundary skipped them.

- **`uapp_file.h` — stateless, no handles, and that is a property of the module, not an
  oversight.** `file-engine/detection` holds no state (pure functions over bytes, const
  registry tables), so this is the one ABI surface with nothing to open, nothing to close, no
  magic cookie, and no use-after-close failure mode. Covers the full registry (format/category/
  mime-type/capability lookups, all total over a bogus integer), the "is detection implemented
  for format X, and if not, why" query path (the file-engine half of the precedent
  `uapp_block_type_is_implemented()` set), and the four detect/describe entry points
  (buffer-first, matching `file-engine/detection`'s own design). One flat `uapp_file_info_t`
  struct covers both C++ result types (`DetectionResult` and `File`) side by side rather than
  mirroring the split, so a caller wanting a confidence AND a checksum makes one call, not two.
- **`uapp_pdf.h` — one opaque `uapp_pdf_document_t` handle, no separate `Library` handle.**
  The C++ layer's `Document` already owns its own `Library` (PDFium's refcounted process-global
  init/destroy) as a member, so exposing `Library` separately would be API surface — a second
  handle type a Kotlin/Swift binding has to bind, document and get the lifetime of right — for
  a benefit (amortising per-document init cost across a tight loop of opens) a document viewer
  does not need: it opens a handful of PDFs per session, not thousands in a loop. Documented as
  a noted tradeoff, not a silent one, and reversible later as a genuinely additive surface if a
  real workload ever needs it. Render is zero-copy through the ABI exactly as in the C++ layer:
  `uapp_pdf_render_page()` writes into a caller-owned buffer (the pixels behind
  `AndroidBitmap_lockPixels()` on Android, `CGBitmapContextGetData()` on iOS) and never
  allocates or copies a bitmap internally — adding a copy at this boundary that the C++ layer
  refuses to do would silently reopen the budget `SP-5-findings.md` closed structurally.
- **`uapp_pdf_is_available()`** (declared in `uapp_capi.h`, implemented in `capi_common.cpp`,
  which is always built) is how a binding asks whether the optional PDF surface exists in THIS
  library before calling anything in `uapp_pdf.h` — `pdf-engine/` builds only when a PDFium is
  vendored (`UAPP_ENABLE_PDF`, AUTO by default), so a fresh clone with no `third_party/pdfium`
  produces a library with no PDF functions in it at all, and calling one would be a link error
  (static) or a missing-symbol abort (shared). The 9 PDF `Status` codes (700-708) exist in
  `uapp_status_t` UNCONDITIONALLY regardless of this, on purpose: a status code whose value
  depends on a CMake switch is not a stable code, and a PDF-less build simply never produces
  one. `uapp_pdf.h`'s own functions are compiled only when `UAPP_PDF_ENABLED` is set by the
  root `CMakeLists.txt`, gated by `target_sources`/`target_compile_definitions` in
  `core/capi/CMakeLists.txt`, not by a runtime branch inside an always-present stub.
- **13 new `Status` codes minted as real `UAPP_STATUS_*` constants** (`FILE_EMPTY` /
  `FILE_PATH_INVALID` / `FILE_OPEN_FAILED` / `FILE_READ_FAILED` at 600-603,
  `PDF_MALFORMED` / `PDF_PASSWORD_REQUIRED` / `PDF_SECURITY_UNSUPPORTED` /
  `PDF_PAGE_NOT_FOUND` / `PDF_PAGE_LOAD_FAILED` / `PDF_RENDER_FAILED` /
  `PDF_RENDER_SIZE_INVALID` / `PDF_BUFFER_TOO_SMALL` / `PDF_NOT_OPEN` at 700-708) — the third
  application of the reachability rule the blocks/search entry below established: these
  thirteen codes existed in `uapp::Status` and were flattened to `UAPP_STATUS_UNEXPECTED` for
  exactly as long as nothing behind the C boundary could produce one. `uapp_file.h`/`uapp_pdf.h`
  end that for all thirteen in one pass, so they are minted rather than kept flattened — not a
  new rule, the same one applied a third time. This is a MINOR bump (0.2 → 0.3): additive, a
  0.2-built caller runs unmodified against 0.3.
- **Tests, real, across all three build configurations this entry's work requires checking**:
  `uapp_file_capi_tests` (12 assertions: the format/capability registry, real-byte detection for
  PNG/JPEG/PDF/plain-text, a renamed file setting `extension_conflicts` without touching
  `format`, DOCX vs XLSX disambiguation through a hand-built ZIP central directory, boundary
  cases for empty/null/malformed input and a bogus enum value) and `uapp_pdf_capi_tests` (10
  assertions: open/count/measure/render through the C API only, a render that honours a padded
  stride — proof row 1 starts at the real stride and not at a tightly-packed `width * 4` — and
  boundary cases for malformed PDF bytes, an undersized render buffer, and use-after-close).
  `ctest` **24/24** (was 20/20) in a normal Debug build and clean under Debug+ASan/UBSan; **19/19**
  with `-DUAPP_ENABLE_PDF=OFF` (the PDF suites are not compiled, not skipped — confirmed no
  `uapp_pdf_*` object file exists in that build), and `uapp_pdf_is_available()` confirmed to
  return `0` in that same PDF-disabled build by linking a standalone check program against it.
  All six public headers (`uapp_capi.h`, `uapp_notes.h`, `uapp_blocks.h`, `uapp_search.h`,
  `uapp_file.h`, `uapp_pdf.h`) confirmed to compile together standalone under
  `clang -std=c99 -pedantic-errors`.

NOT IMPLEMENTED: everything the two wrapped C++ modules already declare out of scope (detection
for the ~35 undetected formats, PDF editing/merge/split/forms/annotation/signatures — see those
modules' own changelog entries), plus a FlatBuffers bulk-transfer payload, a change-notification
mechanism, and concurrent-multi-handle-access proof for these two new surfaces specifically.

### Added — `core/capi`: the page/block and unified-search C ABI (ABI **0.2**, spec §14/§16, §30)

`core/capi` existed but exposed `NoteService` and nothing else, and the previous two entries
both said so: the `core/search` entry recorded "**No C ABI surface** — `core/capi` is
untouched", and the C ABI's own known-gaps bullet read "**Notes only**". A mobile binding
could therefore create and search notes but could not open a page, add a block, or run the
unified query that §16 had just built. This change closes exactly that, and nothing else:
**two new public headers, no change to any existing one's signatures or behaviour.**

| header | surface | handles |
| --- | --- | --- |
| `uapp/capi/uapp_blocks.h` | `blocks::DocumentService` — create/open/rename/delete page, add/update/delete/reorder/move block, counts | `uapp_document_service_t`, `uapp_page_t` |
| `uapp/capi/uapp_search.h` | `search::SearchIndex` — one query across notes **and** pages/blocks | `uapp_search_index_t`, `uapp_unified_results_t` |

`uapp_notes.h` and `capi_notes.cpp` are **byte-for-byte untouched** (`git diff` on both is
empty), which is what makes the version bump additive rather than a claim.

#### The ABI version decision: **0.1 → 0.2, a MINOR bump**

`uapp_capi.h` already documents its own rule — within a major, changes are **additive only**:
"New functions and **new enum values** may appear; existing signatures, enum values and
semantics may not change." Everything here fits: two new headers of new functions, four new
handle types, two new enums, and nine new `uapp_status_t` values. No existing signature, enum
value or semantic moved, so a caller compiled against 0.1 runs unmodified against 0.2 — and
`uapp_abi_is_compatible(0, 1)` still answers `UAPP_TRUE`, which the suite asserts. Major stays
**0** for the reason it always has: the surface is not frozen, and the freeze lands with the
first shipped release along with the ABI-diff CI job (api-design.md §10 rule 5).

#### Judgment call, and a deliberate reversal of precedent: the 5xx status codes

Three commits running (`core/blocks`, `file-engine`, `pdf-engine`) added `uapp::Status` codes
and mapped every one of them to `UAPP_STATUS_UNEXPECTED` in `to_c_status`, minting **no** new
ABI constant. The stated reason, quoted from that code:

> "...nothing reachable through this boundary can return one of them yet — `core/capi` still
> exposes only `NoteService`. Minting an error vocabulary for a block ABI that does not exist
> would freeze a guess into a stable interface."

**The emphasised clause is the whole argument, and `uapp_blocks.h` makes it false.** All nine
block/page codes are now reachable from C: `BlockPositionInvalid` from `add_block`,
`BlockCycleDetected` and `BlockDepthExceeded` from `move_block`, `BlockMalformed` from a
heading level of 9, `PageTitleTooLong` from a long title, and `IdMalformed` from *every* id
argument on the surface. Keeping the old mapping would mean a Kotlin binding cannot tell "you
dragged that block into its own child" from "an unexpected internal condition was contained at
the C ABI boundary" — which contradicts the ERROR DISCIPLINE section of `uapp_capi.h` and spec
§33, makes `UAPP_STATUS_UNEXPECTED` (documented as the boundary's *last line of defence*) the
commonest status a document editor sees, and is unfixable later without a breaking change or a
second parallel error channel.

So **nine codes were minted** — `ID_MALFORMED` (104) and `BLOCK_*` / `PAGE_*` (500–507) — with
the **same numeric values** as `uapp::Status`, each pinned by a `UAPP_CAPI_ASSERT_CODE`
static assert. No code was invented that the core does not have, and no distinction was drawn
finer than the core can actually make.

**The precedent is kept where it still applies:** `file-engine`'s 6xx and `pdf-engine`'s 7xx
codes remain listed-and-flattened to `UNEXPECTED`, because there is still no file or PDF ABI
and nothing reachable through the boundary can produce one. The rule was never "flatten
forever"; it was "do not mint a vocabulary for a surface that does not exist". `core/search`
needed **no** mint at all — its port reports only `SearchQueryEmpty`, `SearchFailed`,
`InvalidArgument` and the `Store*` family, every one already mapped. The test is whether the
*core* grew a code, not whether the *ABI* grew a header.

#### The design decision: block content as a FLAT tagged struct, not a C `union`

`BlockContent` is a `std::variant` of eight payloads, and `core/blocks/block.h` chose a variant
over a class hierarchy specifically because it "maps to C as `{ int32_t type; union { ... }
payload; }` — mechanically, with no ownership question". The C ABI nonetheless uses a **flat**
`uapp_block_content_t`: a discriminant plus the union of every alternative's fields, four in
total (`text`, `language`, `heading_level`, `checked`). Three reasons, in order of weight:

1. **A union breaks the `struct_size` versioning rule.** api-design.md §10 rule 2 versions a
   boundary struct by a leading `uint32_t struct_size`, and that works *because the struct only
   grows at the tail*. A union does not: adding a field to one arm changes `sizeof(union)`,
   which moves every member after it and silently relays out a struct whose `struct_size` a 0.2
   caller is still reporting correctly. Giving `CALLOUT` a payload later would then be a
   **breaking** change wearing an additive change's clothes. Flat, the new field appends,
   `struct_size` says the caller does not have it, and the rule does its job.
2. **The union saves ~16 bytes per *call* and costs every binding a marshalling layer.** Six of
   the eight payloads are one string; nothing stores this struct. Against that, a C union inside
   a struct is the worst-supported construct at an FFI boundary — Swift's C importer maps it to
   an opaque type with no member-wise initialiser, and JNI needs hand-written per-arm
   marshalling, in two bindings, forever.
3. **The union's one real advantage does not survive translation.** In C++ you cannot read the
   wrong `variant` arm; in C nothing stops anyone reading any union member regardless of the
   tag. So the safety is not what is being given up.

**The cost, stated plainly:** the struct carries fields a given `type` does not use, so the
type/field relation is documented and checked at runtime rather than enforced by the compiler.
`uapp_blocks.h` carries the full table, `uapp_block_content_init(type, out)` makes "choose the
type, then the fields" the path of least resistance, and fields a type does not read are
**ignored on input, not validated** — rejecting a stale `language` would turn the ordinary
editor action "turn this heading into a paragraph" into a bug in every caller that reuses its
struct. On output they are zero/empty and `text`/`language` are **never NULL**, so no binding
needs a NULL check before `strlen`.

Three outcomes are told apart on the discriminant, because a UI must react differently to each:
types 0–7 succeed, the eleven declared-but-unbuilt types 8–18 are
`UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED` ("not yet", with a machine-readable reason), and anything
else is `UAPP_STATUS_INVALID_ARGUMENT` ("caller bug").

#### How a tree crosses a C boundary: the flattened pre-order view

`Block` holds `std::vector<Block> children`. The candidates were (a) a handle per block with
`child_count`/`child_at` navigation, (b) a FlatBuffers payload, (c) the tree flattened into
**pre-order** and addressed by index, each row carrying `depth`, `parent_id_hex` and
`child_count`. **(c)**, because it is what the callers already want — UITableView,
RecyclerView and LazyColumn are all index-addressed flat lists, so handing a binding a tree to
re-flatten every frame is the work done twice — and because every string a row exposes is
already materialised inside the page handle, so an accessor borrows (rule B) with no
per-row allocation, exactly as `uapp_search_results_t` already does. No structure is lost:
`depth` + `parent_id_hex` + `child_count` reconstruct the tree, and
`uapp_page_find_block_index()` maps an id back to its row. (a) is the shape `block.h` already
rejects as "where FFI surfaces go to die"; (b) is right for a tree an order of magnitude
larger and stays recorded as NOT IMPLEMENTED. The flattener is **iterative with an explicit
stack**, like every tree walk in `core/blocks`, because the tree comes out of an
attacker-reachable file (spec §28).

#### A real UBSan finding: the enum range pin was only half there

`UAPP_STATUS_FORCE_INT32 = 0x7FFFFFFF` exists so that reading an integer a C caller cast into
the enum is defined rather than UB — and `note_capi_test.c` proves it for `424242`. It was
**wrong for negatives**: an enum whose enumerators are all non-negative has valid range
`[0, 0x7FFFFFFF]`, so `uapp_status_code((uapp_status_t)-1)` was still undefined, and the
asymmetry was invisible because no test had ever cast a *negative* integer in. UBSan flagged it
on `uapp_source_kind_name((uapp_source_kind_t)-1)` the first time the new suite tried.
Fixed by adding `UAPP_*_FORCE_INT32_MIN = -0x7FFFFFFF - 1` to **all three** ABI enums
(`uapp_status_t`, `uapp_block_type_t`, `uapp_source_kind_t`) — purely additive, no signature or
existing value touched — plus a case that casts `{-1, -100, -424242, 424242, INT32_MIN,
INT32_MAX}` into both enums, because an untested half of a claim is not a claim.

#### One behaviour worth pinning: `block_count` on a missing page

`uapp_document_service_block_count()` on a page that does not exist is **`OK` with a count of
zero**, not `NOT_FOUND`: the core's `block_count` counts the rows carrying that page id and
never probes for the page. Surprising enough to be a named assertion and a header note. The ABI
mirrors the core rather than adding an existence check the C++ API does not have — "this layer
adds no behaviour" has to mean it even when the invented behaviour would be nicer.

#### Files

- `core/capi/include/uapp/capi/uapp_blocks.h` — the page/block surface: two handles, 19-value
  `uapp_block_type_t`, `uapp_block_content_t`, `uapp_block_limits_t`, 11 service functions and
  14 page accessors.
- `core/capi/include/uapp/capi/uapp_search.h` — the unified-search surface: two handles,
  `uapp_source_kind_t`, `uapp_search_limits_t`, one query function, six row accessors.
- `core/capi/include/uapp/capi/uapp_capi.h` — ABI 0.2; the 1xx `ID_MALFORMED` and 5xx
  block/page codes; the two `FORCE_INT32_MIN` pins; `UAPP_ID_HEX_LENGTH` and `UAPP_ID_NIL_HEX`
  (shared, because both new surfaces return the nil id rather than NULL).
- `core/capi/src/capi_blocks.cpp`, `src/capi_search.cpp` — one TU per public header, so a
  binding reading `uapp_blocks.h` finds `capi_blocks.cpp` and the eventual shared-library
  export set is a per-surface decision. Compile-time asserts pin all **19** block types and all
  **3** source kinds to their C++ values, and `UAPP_BLOCK_POSITION_APPEND` to
  `blocks::kAppendPosition`. The `to_block_content` switch lists the eleven unbuilt types
  explicitly rather than defaulting, so implementing one in the core without teaching the
  translator is a compile error.
- `core/capi/src/capi_internal.h` — four new handle magic cookies, and
  `UAPP_CAPI_GUARD_END_FALLBACK` for the three exported pure lookups that return a value
  rather than a status.
- `core/capi/src/capi_common.cpp` — `to_c_status` extended, with the reversal argued in place;
  the two status-string switches extended to stay exhaustive.
- `tests/capi/capi_test_harness.h` — the ~70-line C harness, extracted so three C test files
  do not carry three copies of the macros that decide whether a test passed.
  `note_capi_test.c` deliberately keeps its inline copy: it is a passing suite guarding a
  frozen surface, and a non-functional rewrite of it is the more expensive risk. Recorded in
  the header rather than left for a reviewer to notice.
- `tests/capi/blocks_capi_test.c`, `tests/capi/search_capi_test.c`, `tests/CMakeLists.txt` —
  two new **C** executables and four CTest entries (`uapp.capi.blocks.slice` / `.boundary`,
  `uapp.capi.search.slice` / `.boundary`).
- `core/capi/CMakeLists.txt` — the two new sources; the composition-root comment updated, since
  this target now binds three ports to their SQLite adapters rather than one.

#### Verification

```text
$ cmake -S . -B build_capi2 -DCMAKE_BUILD_TYPE=Debug && cmake --build build_capi2 -j8
[100%] Built target uapp_blocks_capi_tests   # clean under -Werror -fno-exceptions -fno-rtti
[100%] Built target uapp_search_capi_tests

$ ./build_capi2/bin/uapp_blocks_capi_tests
23 passed, 0 failed
$ ./build_capi2/bin/uapp_search_capi_tests
13 passed, 0 failed

$ ctest --test-dir build_capi2 --output-on-failure
100% tests passed out of 20

$ cmake -S . -B build_capi2_asan -DCMAKE_BUILD_TYPE=Debug -DUAPP_ENABLE_SANITIZERS=ON
$ UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build_capi2_asan
100% tests passed out of 20          # ASan + UBSan, no findings
```

The standalone-C99 property, checked per header rather than only via the test binaries —
`architecture.md` §8 rule 5 asks that each capi header "compile clean under a C compiler, not
just a C++ one", and a header that only works *after* another one is included does not:

```text
$ clang --version | head -1
Apple clang version 21.0.0 (clang-2100.3.34.2)

$ for h in uapp_capi uapp_notes uapp_blocks uapp_search; do \
    printf '#include "uapp/capi/%s.h"\nint main(void){return 0;}\n' $h > only_$h.c; \
    clang -std=c99 -pedantic-errors -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion \
          -Wshadow -Wcast-qual -Wdouble-promotion -Wformat=2 -Wundef -Wunused -Werror \
          -Icore/capi/include -fsyntax-only only_$h.c && echo "OK: $h.h alone, as C99"; done
OK: uapp_capi.h alone, as C99
OK: uapp_notes.h alone, as C99
OK: uapp_blocks.h alone, as C99
OK: uapp_search.h alone, as C99
```

This check is what caught the first spelling of `UAPP_BLOCK_POSITION_APPEND` as `((size_t)-1)`:
a C-style cast, which `-Wold-style-cast -Werror` forbids in C++, so the implementation could
not consume its own public macro. A macro only the C side can use is not an ABI. It is
`SIZE_MAX` now.

```text
$ for b in uapp_blocks_capi_tests uapp_search_capi_tests uapp_capi_tests; do \
    leaks --atExit -- ./build_capi2/bin/$b | grep "leaks for"; done
Process 89567: 0 leaks for 0 total leaked bytes.
Process 89572: 0 leaks for 0 total leaked bytes.
Process 89577: 0 leaks for 0 total leaked bytes.
```

Unchanged counts for every pre-existing suite: **notes 42, blocks 36, search 22, file_engine
65, pdf 23, and the note C ABI still 17** — the last one is the number that matters, since it
is the evidence that this change is additive.

The 36 new C cases cover, through the **public headers only**: a page with one block of every
implemented type, two of them nested, read back and verified field by field (type, depth,
parent id, direct child count, payload, timestamps, pre-order position); update with and
without a type change, children surviving it, and a rejected update leaving the block
untouched; delete cascading through a three-level subtree with the sibling gap closed and the
sibling surviving; reorder to the front, to the end via the append sentinel, and a rejected
out-of-range reorder leaving the order intact; move re-parenting a block *and its subtree* with
depths recomputed, out to the top level via both `NULL` and the explicit nil id; rename and
delete page; persistence across close/reopen; two independent services sharing nothing; a
read-only service reading but failing writes with `STORE_READ_ONLY`; **one query returning a
note and a block together**, each typed, the block hit carrying the page id *and* the page's
title; a page-title hit; the index following every content edit, rename and delete with no
stale hit, from both surfaces, down to an empty index; limit clamping; and a results handle
outliving the index it came from.

Boundary coverage matches `note_capi_test.c`'s rigor: null handles on every entry point; null
strings and null out-params, with the optional ones proved optional; `struct_size` rejected
below this build's and accepted above it, on configs, limits **and** the content struct;
use-after-close on all four handle types; malformed ids told apart from absent ones; row-index
bounds including `(size_t)-1` and an empty page; move cycles (self, child, grandchild),
cross-page moves, bad positions; the full discriminant matrix; heading levels, control bytes in
a language tag and over-length input; 13 hostile queries; and **wrong-type handles across
surfaces** — including a live *note* handle where a page is expected, and
`uapp_search_results_t` versus `uapp_unified_results_t`, which are the two that will sit side
by side as indistinguishable `Long`s in one Kotlin binding. The destructors are shown
*refusing* to free a handle that is not theirs and leaving it intact, because leaking beats
freeing someone else's live object.

Two **mutation checks**, to prove the suites are not vacuous rather than assuming it:

```text
# row.depth := 1, row.child_count := 0  (the flattened tree loses its structure)
3 cases red: "every implemented type", "update ... change its type", "move re-parents"

# a block hit's container_id := nil      (the hit can no longer be navigated to)
1 case red: "one query returns a note and a block together"
```

A third attempted mutation — moving the child-stack push ahead of the row emission — was
**discarded as a non-mutation**: it does not change emission order, so a green suite was the
correct answer rather than a gap. Recorded because a mutation check that cannot fail is worth
exactly as little as a test that cannot fail.

#### NOT IMPLEMENTED (spec §5) — deferred, with reasons

- **~~No C ABI for `file-engine/detection` or `pdf-engine/reader`.~~** *(CLOSED by the ABI 0.3
  entry below: `uapp_file.h` and `uapp_pdf.h` now cover both, their `Status` codes are minted
  as real `UAPP_STATUS_FILE_*`/`UAPP_STATUS_PDF_*` constants on the same reachability
  reasoning this entry used for blocks/search, and `core/capi` now has a C ABI surface for
  every module that exists in the repository.)*
- **No OCR, scanner, sync or AI ABI.** Those engines do not exist.
- **Deferred: `append_block` and `get_block`.** `DocumentService` has both;
  `uapp_document_service_add_block(..., UAPP_BLOCK_POSITION_APPEND, ...)` is `append_block`
  exactly, and a single block is read out of a page snapshot, so neither would add reach — only
  surface area to freeze.
- **NOT IMPLEMENTED: a FlatBuffers block-tree payload** (`uapp_buf_t`, api-design.md §10
  rule 4). The flattened-handle view is the right shape at today's sizes; the argument and the
  point at which a buffer wins instead are both in `uapp_blocks.h`.
- **NOT IMPLEMENTED: change notification.** A page handle is a snapshot, so a binding learns
  about a mutation by reopening. No observer, no diff, no incremental row update — which a real
  editor UI will eventually want, and which needs a change-feed the core does not have.
- **Not proven: concurrent use from multiple threads.** Distinct handles share no mutable
  state and the suite shows independent services not seeing each other, but nothing here runs
  two threads. Unchanged from the 0.1 position.
- **Unchanged from 0.1:** no `uapp_error_t` cause chain, no job protocol or platform vtable, no
  shared-library packaging or ABI freeze, no ABI-diff CI job, and no bindings — no
  `uapp-kotlin`, no `UAPPKit`, no JNI or Swift code. This run widened the boundary those will
  attach to and stopped there.

---

### Added — `core/search`: one unified full-text search across notes and pages/blocks (spec §16, Phase 9)

Spec §16 asks for one search over "Notes, Pages, PDFs, DOCX, Images through OCR, scanned
documents, file names, metadata, databases", and for it to "work offline for locally available
data". Before this change the product had **per-store** search only: `notes` had a working FTS5
index and `NoteStore::search`, while `sqlite_block_store.cpp` carried an explicit
`NOT IMPLEMENTED: full-text indexing of block text`. There was no query that could see both.
This change closes that: **one query string, results from notes and from pages/blocks, each hit
self-describing about which kind of object it is and which ids open it**. Offline by
construction — it is FTS5 in the local SQLite file, with no network path anywhere in it.

Scope is deliberately the two content models that exist. §16's items 3–6 and its PDF/DOCX/OCR
source kinds are listed under **NOT IMPLEMENTED** below, with reasons.

#### The design decision: ONE shared FTS5 table, not two tables merged in the application

The two candidate shapes were (a) a single index table that every content writer writes into,
addressed by `source kind + source id + container id`, and (b) a second `block_fts` beside the
existing `note_fts`, queried in parallel and merged in C++. **(a)**, for a reason that is not
about convenience:

> **bm25 scores from two different FTS5 tables are not comparable.**

`bm25()` is derived from *that index's* corpus statistics — row count, average row length,
per-term document frequency. A note corpus and a block corpus have wildly different average row
lengths (a note body may be 4 MiB; a paragraph block is a sentence), so the same term scores
differently in each. Merging two such lists means inventing a fusion heuristic — score
normalisation, or reciprocal-rank fusion — and then owning its calibration forever, with no
ground truth to calibrate against. With one table, cross-kind ranking is a single
`ORDER BY bm25(...)` over one set of statistics: correct by construction rather than tuned.
Secondary but real: one table is one tokenizer configuration, which matters when the ICU/CJK
tokenizer lands (`database-design.md` §3.9 tracks it) — two tables is two places to forget.

This is also the shape `database-design.md` §3.9 already specified (`fts_content` with
`object_kind` / `object_id` columns), so the decision is a follow-through, not a new direction.

**One deviation from that sketch, argued rather than assumed:** §3.9 puts the ids inside the
FTS5 table as `UNINDEXED` columns. They are split out here into an ordinary `search_entry`
table joined to `search_fts` by rowid — exactly as `note` / `note_fts` already are. An
`UNINDEXED` FTS5 column can carry neither a `CHECK` constraint nor an index, and index
*maintenance* is entirely by id ("replace the row for block X", "drop everything whose
container is page Y"); against `UNINDEXED` columns each of those is a full scan of the index,
and with `search_entry` each is a B-tree lookup (`UNIQUE(source_kind, source_id)`,
`ix_search_entry_container`). It also lets the id columns carry the same `length(id) = 16`
CHECK that `note` and `block` already do (spec §28).

#### The granularity decision: BLOCK-level, not page-level

A page could be one index row holding all of its blocks' text concatenated. That is simpler and
it is wrong here, for three reasons:

1. **Write amplification.** A page holds up to `kMaxBlocksPerPage` (50 000) blocks. Editing one
   paragraph would mean re-reading the page, re-concatenating it and rewriting one FTS row
   covering every block — O(page) work inside the write transaction on every edit. Block-level
   rows make the same edit O(1).
2. **The result would not be actionable.** `snippet()` over a concatenated blob says the match
   is "somewhere on page Y" with no way to map the offset back to a block id. A user searching
   a 400-block page then finds the match by eye. Block-level rows let a hit carry **both** the
   block id (where to scroll) and the page id (what to open), which is what
   `search::SearchResult` exposes and what `tests/search/unified_search_test.cpp` asserts.
3. **Deletes.** Removing one block from a page-level row means rebuilding that row from the
   surviving blocks. Removing a block row is a delete.

Page **titles** are indexed too, as their own `SourceKind::Page` row: a page title is in no
block, so without it searching for a page by name would silently fail. A block hit joins to
that row for its display title, which is why renaming a page relabels all of its hits without
rewriting any of them (asserted).

**The cost, stated plainly:** each row is matched independently, so a two-term query whose terms
sit in two *different* blocks of one page does not match that page. (It does match a note, whose
title and body are one row.) The fix — a page-level roll-up row *in addition to* the block rows
— reintroduces exactly the write amplification above and is **NOT IMPLEMENTED**; see the list
below.

#### Index maintenance: in the content writer's transaction, never a catch-up pass

`database-design.md` principle 7 is "index and content commit together"; §3.9 adds that "a
separate indexer thread that 'catches up' is how search silently rots". So the port
(`core/search/search_index.h`) is **read-only by design** — there is no `index()` or `reindex()`
method a caller could forget to call. The only way into the index is to store a note, page or
block, and every one of those paths writes its index row inside the transaction it already had:

- `sqlite_note_store.cpp` — `insert` / `update` / `remove`, following the precedent it already
  set for `note_fts`.
- `sqlite_block_store.cpp` — `create_page` (which gained a transaction: it was a single INSERT
  and now writes two rows), `set_page_title`, `delete_page`, `insert_block`, `update_block`,
  `delete_block`. `move_block` deliberately writes nothing: a move changes `parent_id` and
  `sort_index`, neither of which is in the index, and the one thing it *would* change — the
  container, on a cross-page move — is the case the block store already rejects as
  NOT IMPLEMENTED.

Two subtleties that are load-bearing rather than incidental, each with a mutation-tested
regression case:

- **Subtree deletes index-first.** `delete_block` cascades to the whole subtree, and finding
  those index rows means walking `block.parent_id` — which the content delete is about to
  remove. Running the index walk second would leave every descendant's text matchable forever
  and *nothing would report an error*.
- **Within a delete, FTS rows before addressing rows.** The two tables are joined by rowid and
  SQLite reuses the rowids of deleted rows. Deleting the addressing row first orphans the FTS
  row; the orphan is invisible to readers (the query INNER JOINs `search_entry`) but sits on a
  rowid the next insert is handed, so the deleted object's text comes back attached to a
  different object's entry. Both orderings were verified by inverting them and watching the
  suite go red.

A block whose plain text is empty — a `DividerBlock`, or a paragraph the user cleared — gets no
entry at all and is erased if it had one. Keeping empty rows would inflate the index, make
`entry_count` useless as a staleness check, and skew bm25's average-row-length weighting for
every real hit.

#### Query interface

`search::SearchIndex::query(query_text, limit, out)` returns ranked `SearchResult`s carrying:
the `SourceKind` tag (`note` / `page` / `block`), `source_id`, `container_id` (the page, for a
block), the containing object's `title`, a `snippet` with matched terms delimited by `[` `]`,
and a `rank`. `snippet()`'s column argument is `-1` ("the leftmost column that matched"), which
is what lets one statement serve all three kinds — a page entry has text only in `title`, a
block only in `body` — instead of a per-kind query returning empty snippets for two kinds out
of three.

`core/search` links `uapp::core_common` **and nothing else** — not `core_notes`, not
`core_blocks`. A hit is described by a source-kind tag plus the shared 16-byte `ObjectId`, so
the search layer does not link the domains it searches; otherwise every future source kind
(PDF page, OCR region, file name) would add one more domain dependency until search linked the
whole product. The link line enforces that, not a comment.

**Query safety reuses the existing discipline rather than reinventing it.** Raw user text goes
through the same `build_fts5_match_expression` allow-list builder (`fts_query.cpp`) that
`NoteStore::search` already uses: word bytes only, each surviving term emitted as a
double-quoted FTS5 string, ANDed, last term prefix-matched. A `static_assert` ties
`search::kMaxQueryBytes` to `notes::kMaxQueryBytes` so the two caps cannot drift. The hostile
input test drives the *fts_query* corpus **end to end through the unified port** — the point
being not that the builder is safe (that was already proved in isolation) but that the new layer
did not route around it.

#### Unified search requires the stores to share one database file

There is one index table, so a query sees notes **and** pages/blocks only when the note store,
the block store and the index are opened on the same `path`. That is `architecture.md` §3
Revision 2 ("there is exactly one persistence port … document, file, search, AI-metadata and
sync modules all go through it"), not a shortcut: the two stores' tables are disjoint and they
version themselves under different `schema_meta` keys, so they already coexist in one file.
Opened on *different* files, each file holds an index over its own content and a query returns
that file's hits — which degrades honestly rather than returning half an answer.

`":memory:"` **does not work for unified search**, and the header says so loudly: each
`sqlite3_open` of `":memory:"` is a private database, so two stores on `":memory:"` cannot see
each other's rows and the symptom is zero results with no error. The pre-existing per-store
suites keep using `":memory:"` unchanged (each gets its own index, covering its own content);
the unified suite uses one real file per case in a self-removing `TempDir`.

#### Files

- `core/search/include/uapp/search/{search_result.h,search_index.h}`, `src/search_result.cpp`,
  `CMakeLists.txt` — the port and its value type.
- `storage/sqlite/src/search_index_sql.{h,cpp}` — the shared DDL and the maintenance
  primitives (`put` / `erase` / `erase_container` / `erase_block_subtree` / `entry_count`),
  internal to the target. Free functions taking a `sqlite3*` on purpose: the caller must
  already be inside its own content transaction, and handing them a connection rather than an
  object makes that relationship visible at every call site.
- `storage/sqlite/{include/uapp/storage/sqlite/sqlite_search_index.h,src/sqlite_search_index.cpp}`
  — the read side. Opens read-only by default (the port cannot write, so the connection holds
  no write permission) and *probes* for the index tables at open, so pointing it at a path no
  store ever wrote is `StoreOpenFailed` rather than "no results" forever.
- `tests/search/unified_search_test.cpp`, `tests/CMakeLists.txt` — the suite and four CTest
  entries (`uapp.search.unit` / `.edits` / `.deletes` / `.hostile`).
- `tests/support/test_framework.h` — added `UAPP_REQUIRE_MSG` (fatal, with a computed detail
  string), needed wherever the interesting information is *what was actually there* — a result
  list, a count — rather than the expression that failed.
- `storage/sqlite/src/sqlite_{note,block}_store.cpp` — index maintenance wired into the
  existing transactions; the block store's `NOT IMPLEMENTED: full-text indexing` comment is
  replaced by the reasoning for what it now does.

#### Verification

```text
$ cmake -S . -B build_search -DCMAKE_BUILD_TYPE=Debug && cmake --build build_search -j8
[100%] Built target uapp_capi_tests          # clean under -Werror -fno-exceptions -fno-rtti

$ ./build_search/bin/uapp_search_tests
22 passed, 0 failed

$ ctest --test-dir build_search --output-on-failure
100% tests passed out of 16

$ cmake -S . -B build_search_asan -DCMAKE_BUILD_TYPE=Debug -DUAPP_ENABLE_SANITIZERS=ON
$ UBSAN_OPTIONS=print_stacktrace=1 ctest --test-dir build_search_asan --output-on-failure
100% tests passed out of 16          # ASan + UBSan, no findings
```

Unchanged counts for every suite whose storage this work touches: **notes 42, blocks 36** (both
still green with the extra index writes inside their transactions), file_engine 65, capi 17,
pdf 23. The 22 new cases cover: a query matching only a note; only a block; only a page title;
one matching **both** stores with each hit correctly typed and both ids present; rank ordering
and the `limit` contract; editing a note, editing a block and renaming a page each replacing
what search finds with **no stale hit**; a type change to `Divider` removing the entry;
deleting a note, a block **and its subtree**, and a whole page; delete-then-re-add not
resurrecting old text (the rowid-reuse case); a non-matching query returning `Ok` + empty; an
empty/punctuation-only query returning `SEARCH_QUERY_EMPTY` *distinctly* from "no results"; 20
hostile queries (`NEAR(`, unbalanced quotes, `col:value`, `'; DROP TABLE search_entry; --`,
quote-escape attempts, control bytes) proving the injection surface stays closed and the index
survives; an oversized query rejected rather than truncated; an embedded NUL; survival across
close/reopen; and the read-only-open-with-no-index error.

Two **mutation checks** were run to prove the suite is not vacuous rather than assuming it:
removing the subtree index-erase, and inverting the FTS-rows-first delete ordering. Each turned
the suite red at the specific case that claims to cover it.

#### NOT IMPLEMENTED (spec §5) — deferred, with reasons

- **§16.3 Fuzzy search.** Needs the trigram auxiliary table over titles plus a bounded
  Levenshtein rescore of the top-N FTS candidates that `database-design.md` §3.9 specifies. Out
  of scope for this run by instruction; the §3.9 note that fuzzy matching is **not** attempted
  over full body text still stands as the design.
- **§16.4 Metadata search.** There is no metadata to search yet — no tags, no properties, no
  workspaces, and `page` has no `workspace_id` column. A metadata query language over fields
  that do not exist would be fiction.
- **§16.5 Semantic search and §16.6 AI-powered search.** No embeddings, no vector store, no
  `embedding` / `embedding_meta` tables, no RRF hybrid fusion. Explicitly out of scope this
  run; `architecture.md` §6 sketches the eventual
  `search/hybrid (FTS5 BM25 ∪ vector top-k → RRF fusion)`, and ADR-level sign-off on an
  on-device model is a prerequisite rather than a detail.
- **Search over PDF, DOCX, images-through-OCR, scanned documents, file names.** Nothing to
  index: `file-engine/detection` produces a `File` value and extracts no text content from
  anything, and `pdf-engine/reader` is viewing-only with no text extraction. The index schema
  needs no change to accept them — a new `SourceKind` value plus an indexer — but no
  enumerator is pre-allocated for them, for the same reason `BlockType` does not fake its
  unimplemented types: a persisted value with no writer behind it is a promise the enum cannot
  keep.
- **Database/collection search (§15).** The `Database` block type is itself NOT IMPLEMENTED.
- **A page-level roll-up row.** Discussed above: a multi-term query spanning two blocks of one
  page does not match that page today. Deferred because the fix pays the O(page) write cost
  that block-level granularity was chosen to avoid; the honest resolutions are either the
  roll-up row on a debounced background write (which breaks the one-transaction rule) or a
  query-time post-pass that groups block hits by `container_id` and re-scores.
- **Collapsing `note_fts` into the unified table.** Notes are currently indexed **twice** —
  once in `note_fts` for `NoteStore::search`, once in the unified table. That is a real cost
  (`database-design.md` §9 budgets the FTS index at ~40 % of text, so notes pay it twice) and
  it is deliberate: `NoteStore::search` returns `notes::SearchHit` with its own bm25 ranking
  and is consumed through `core/capi`, which this change is not permitted to touch. Repointing
  it would change result ordering across a frozen ABI. The follow-up is to reimplement
  `NoteStore::search` as a kind-filtered unified query and delete `note_fts`, once the C ABI
  moves onto the unified API.
- **~~No C ABI surface.~~** *(CLOSED by the ABI 0.2 entry at the top of \[Unreleased], which
  added `uapp/capi/uapp_search.h`. Was: "`core/capi` is untouched, by instruction and for the
  same reason `core/blocks` and `file-engine` landed without it: the ABI is versioned and
  permanent, and committing to a search ABI before the layer has settled is how a bad shape
  becomes forever." The layer settled; the ABI followed.)*
- **A CJK-capable tokenizer.** `unicode61` does not segment Chinese/Japanese/Korean, so a CJK
  query degrades to whole-run matching. Inherited from `note_fts` rather than introduced here;
  the fix is the custom ICU tokenizer `database-design.md` §3.9 names.
- **No CLI surface.** `tools/notes_cli` still drives `NoteStore::search` only; there is no
  `uapp-notes search-all`. The port is exercised by the test suite, not by a shipped command.
- **Concurrency testing.** Two connections on one file, both taking `BEGIN IMMEDIATE` under a
  5 s busy timeout, is the WAL arrangement `architecture.md` §6 describes, but no test drives a
  note write and a block write at the same instant. Untested, therefore not claimed.
- **Index rebuild / integrity check.** The index is derived, rebuildable data
  (`database-design.md` §6 marks `fts_content` as such), but there is no `REBUILD` path and no
  "does the index agree with the content" verifier. §11 of that document names derived-data
  drift as a known integrity hazard, and the one-transaction discipline is a preventive measure,
  not a detector.
- **No fuzz target.** `testing-strategy.md` §6.1 does not list one for search, and the one
  untrusted input here — the query string — is already covered by the `fts_query` unit tests
  and the 20-case hostile corpus above. Recorded so the absence is a decision, not an oversight.

### Added — `fuzz_pdf_reader`: a libFuzzer target over `pdf-engine/reader`, and the product bug it found (spec §29)

Closes the gate the `pdf-engine/reader` entry below left open — its own "what is still owed"
list names "a fuzz target for the PDF parsing path" — and it is the target
`testing-strategy.md` §6.1 lists **first**, at P0, as "largest attack surface".
`docs/risk-register.md` treats the PDF parser as the highest-risk parser in the product.

- **`tests/fuzz/fuzz_pdf_reader.cpp`** — one harness covering **two** §6.1 rows,
  `fuzz_pdf_parse` and `fuzz_pdf_render`. The rationale §6.1 gives for listing them separately
  ("rendering is a separate code path from parsing") is real, but `pdf-engine/reader` is one
  library behind one door, so two binaries would mean two processes re-parsing the same bytes
  to reach the second half of the same call chain. Every iteration therefore does both: open
  the bytes, count the pages, measure them, rasterise page 0 into a caller-owned buffer.
  `fuzz_pdf_content_stream` stays unbuilt — there is no first-party content-stream entry point
  to call. The fuzzed door is `Document::open_buffer`, which is not merely the untrusted-upload
  shape but the **only** door: `document.h` design rule 1 is "buffer first, path never".
- **The input is the PDF file, verbatim** — the same constraint `fuzz_file_detection.cpp`
  adopted, for the same three reasons. The two things a PDF's bytes cannot carry, the
  **password** and the **render geometry**, are derived deterministically from the input's own
  first byte, exactly as the detector's harness derives a filename.
- **An oracle over the `Status` sets `document.h` states in prose, and only those.** A failed
  open leaves no half-open `Document`; `page_count()` is never negative; index `-1` and index
  `page_count` are always `PdfPageNotFound` on both page-level entry points; a geometry
  `required_buffer_bytes()` accepted is never then rejected by `render_page`; a successful
  render never touches the caller's **row padding**. Deliberately not asserted: positive page
  dimensions, any particular recovered page count, or that a broken content stream fails —
  `document.h` promises none of those and PDFium's xref rebuild makes all three legitimately
  variable. Inventing a rule stricter than the header would manufacture a fake defect.
- **`pdf-engine/` is no longer skipped under `UAPP_ENABLE_FUZZING`.** `storage/` and
  `core/capi` still are: they sit off the untrusted-input path and only cost build time. The
  PDF reader *is* the untrusted-input path, so under fuzzing it is a library under test rather
  than an extra. The `AUTO` contract is intact — with no vendored PDFium the target simply does
  not exist and the fuzz build still produces `fuzz_file_detection`.
- **A 10-file seed corpus** in `tests/fixtures/corpus/fuzz_pdf_reader/`. Eight are the
  **byte-identical output of `tests/pdf_engine/pdf_fixture.h`**, so fixture and seed cannot
  drift apart: the valid two-page document, a bare `%PDF-` header with no body, a `startxref`
  pointing past EOF, four truncations, and the new regression fixture. The ninth is a PDF whose
  trailer claims an `/Encrypt` dictionary (standard handler, V1/R2) — the only seed that
  reaches PDFium's security handler and the only one that drives the password overload of
  `open_buffer`. The tenth is the raw reproducer below. Every seed's behaviour was checked
  against the reader *before* the first run, which is exactly the control the detector work's
  corpus-generator bug argued for.

**A REAL PRODUCT BUG, FOUND AT 139 EXECUTIONS AND FIXED.** The mutator turned the valid seed's
page tree from `/Kids [3 0 R 4 0 R]` into `/Kids [3 0 R 44 0 R]` and left `/Count 2`. Object 44
does not exist, so `page_count()` returned 2 and index 1 became simultaneously *in range* and
*unresolvable* — a combination no well-formed file can produce and no hand-written case had
covered. `page_size()` and `render_page()` both answered it through the document-load error
mapper, which reads PDFium's process-global `FPDF_GetLastError()`. Neither
`FPDF_GetPageSizeByIndexF` nor `FPDF_LoadPage` writes that global, so it still held
`FPDF_ERR_SUCCESS` from the load that had just succeeded, and the mapper turned "no error" into
**`Status::Internal`**. Two defects in one: `Internal` is outside both functions' documented
return sets, so a caller switching on them fell through to its default branch; and `status.h`
defines `Internal` as *"a bug in us, not in the caller's input"* for a condition caused
entirely by the caller's input — so attacker-supplied bytes could make the product report an
internal defect, lying to whoever reads the log and poisoning any error-rate alert over it.
**The fix** is two call sites: both page-level failures now return `Status::PdfPageLoadFailed`
(704, "index in range, but the page object is unusable") unconditionally instead of consulting
a global nothing wrote. Nothing is lost, because there was nothing there. The mapper is now
document-load-only and documented as such, and the two arms of it that could still produce an
undocumented status out of `open_buffer` are left alone deliberately — believed unreachable,
and *checked* rather than asserted, because the oracle holds every `open_buffer` return against
the documented set on every input.

**Tests: `pdf_engine` 22 → 23 cases** — one added, `pdf.malformed an in-range page that will
not load is PDF_PAGE_LOAD_FAILED`, over a new `dangling_page_reference_pdf()` fixture rebuilt
from source per `pdf_fixture.h`'s no-opaque-blobs rule rather than checked in as the reproducer
blob. Confirmed failing with `Internal` against the pre-fix build and passing after; the raw
reproducer is a corpus seed as well. Page 0 of the same file is asserted still intact and the
`PdfPageNotFound` boundary still distinct, so the fix cannot have collapsed "out of range" and
"in range but broken" into one answer.

**The runs, actually performed** (LLVM 23.1.2, Debug, ASan+UBSan; full record in
`testing-strategy.md` §6.4): three bounded 121 s runs after the fix — 1 687 510, 1 924 058 and
1 821 616 executions at 13 946 / 15 901 / 15 054 exec/s. **5 433 184 executions over 363 s:
zero crashes, zero hangs (slowest unit < 1 s), zero sanitizer reports, zero oracle
violations.** Coverage plateaued at 316 edges / 481 features with the working corpus at 21–22
units; raising libFuzzer's length cap to 128 KiB added nothing.

**The honest limit, stated because those numbers look better than they are.** The vendored
PDFium is a **prebuilt dylib** with no SanitizerCoverage and no sanitizer instrumentation, and
nothing the harness can do adds any. So libFuzzer's feedback comes from *our wrapper* — 316
edges is the wrapper's size, not PDFium's — and the mutator is close to blind past the first
branch that matters. ASan still sees PDFium's heap through the malloc interceptor (double-free,
use-after-free, an overflow caught by an intercepted libc call), but a plain out-of-bounds read
inside uninstrumented PDFium code is invisible, and UBSan sees nothing there at all. What this
target genuinely covers is our layer: the status mapping, the `Library` refcount, the bounds
arithmetic, and — because the render destination is an **exact-fit** heap allocation flanked by
ASan redzones — any write by PDFium outside the geometry `RenderTarget` authorised. That last
one is the memory-safety property this module actually promises. The rest waits on the hermetic
from-source PDFium build SP-1 asks for, already on the bootstrap's owed list below.

**Verification.** The fuzz build configures and builds clean at `-Werror` under LLVM 23. The
normal build is untouched: a from-scratch `cmake -S . -B build_check && cmake --build
build_check && ctest` with `UAPP_ENABLE_FUZZING` at its default OFF and `UAPP_ENABLE_PDF` at
`AUTO` gives **ctest 12/12** and unchanged counts for every suite this work does not touch —
notes 42, blocks 36, file_engine 65, capi 17 — with pdf at 23 for the one added regression case,
and no `-fsanitize` flag anywhere in `compile_commands.json`.

**NOT IMPLEMENTED**: `fuzz_pdf_content_stream` (no entry point to call), and the other fifteen
§6.1 targets, which still name empty Phase 0 directories. Also still absent: the CI wiring for
§6.2's cadence, and structure-aware mutation — a naive byte mutator on a PDF spends most of its
cycles producing files that die at the header, and the modest edge count above is partly that.

### Added — `pdf-engine/reader`: PDF viewing bootstrap on a vendored PDFium (spec §11)

**Status: BOOTSTRAP, not the shipping arrangement.** This is the open / page-count /
page-size / render-to-caller-buffer slice of spec §11 (viewing only — no editing, merge/split,
forms, annotation, or signatures), built against a **prebuilt** PDFium binary so the reader
wrapper's own logic could be written and tested now, while the real answer SP-1 recommended —
a hermetic from-source build producing our own static archive for every target platform — is
still owed before this ships. Nothing here should be read as "PDF support: done."

- **`third_party/pdfium/`** — PDFium 156.0.8076.0 (`chromium/8076`) from
  `bblanchon/pdfium-binaries`, pinned by tag **and** SHA-256 (upstream ships no release
  tarballs or semver tags, so the tag alone does not identify the bytes). `fetch_pdfium.sh`
  downloads and verifies it; the binary itself is `.gitignore`d, not committed, and is only
  fetched for `mac-arm64` — `android-arm64`/`ios-arm64` are deliberately absent so this cannot
  be mistaken for a mobile solution. Build args cross-checked against `args.gn` in the archive:
  `pdf_enable_v8=false`, `pdf_enable_xfa=false`, `pdf_use_partition_alloc=false`, matching
  SP-1's findings; AGG (not Skia) confirmed as the rasterizer via `licenses/agg23.txt`.
- **Deviation from SP-1, recorded rather than hidden**: this release ships `libpdfium.dylib`
  (shared), not a static archive — no `pdf_is_complete_lib=true` variant exists for macOS in
  this release. The CMake target is `IMPORTED SHARED`, not `IMPORTED STATIC`; the *interface*
  SP-1 asked for (consumed via an `IMPORTED` CMake target) is honoured, the artefact type
  changes once the hermetic build produces our own archive.
- **`CMakeLists.txt`**: `UAPP_ENABLE_PDF` tristate (`AUTO`/`ON`/`OFF`). `AUTO` builds
  `pdf-engine/` iff a vendored PDFium is found and says why not otherwise; `ON` fails configure
  outright if it's missing, rather than silently dropping the PDF tests from a "green" build.
- **`pdf-engine/reader/`**: buffer-first (not just path-first) open, matching the file-engine's
  design; page count; per-page `MediaBox` dimensions; render into a **caller-owned** RGBA8888
  buffer (zero-copy per SP-5 — the wrapper writes into memory the caller supplies, it does not
  allocate and hand back a copy). `FPDF_InitLibrary`/`FPDF_DestroyLibrary` lifecycle wrapped in
  a refcounted `Library` so it is safe across multiple `Document`s and multiple test runs in
  one process.
- **9 new `Status` codes (700-708)**, deliberately coarse: PDFium's own `FPDF_GetLastError()`
  distinguishes about six failure classes, so finer-grained codes here would be invented, not
  real. `core/capi`'s exhaustive `to_c_status` switch extended to list-and-map all 9 to
  `UAPP_STATUS_UNEXPECTED` (no new ABI constant minted — same pattern as the blocks and
  file-engine commits, for the same reason: nothing reachable through the C ABI can produce
  one of these yet).
- **Licensing**: verified against the files actually inside the fetched archive, not assumed.
  PDFium itself is BSD-3-Clause (`licenses/pdfium.txt`), matching `file-format-support.md`'s
  existing "ADOPT" row. AGG 2.3 (`licenses/agg23.txt`) is the permissive pre-2.5 line, not the
  GPL 2.5+ line — evidence, not clearance; the residual risk is carried forward in
  `THIRD_PARTY_NOTICES`, not resolved here. The distributor's own build scripts are MIT.
  11 further bundled third-party notices (abseil, freetype, icu, libjpeg_turbo, zlib, ...)
  copied into `third_party/pdfium/licenses/` and indexed in `THIRD_PARTY_NOTICES`.

**Tests (real, 22 assertions, `uapp.pdf.read`/`uapp.pdf.render`/`uapp.pdf.malformed`, ctest
12/12 including the 9 pre-existing suites unchanged)**: open a valid two-page PDF from a
buffer; correct per-page `MediaBox`; move semantics; idempotent close; `Library` refcounting
returns to zero; render writes real (non-blank) pixels into the caller's buffer; **a padded
stride is honoured and the write stays inside the buffer bounds** (the fix for a real bug
found mid-development: PDFium requires a 4-byte-aligned stride, and an unaligned caller-chosen
stride was not being rejected before this); repeated renders over the same buffer agree; a
genuinely blank page renders as background, not uninitialized memory; empty/null/truncated/
non-PDF/malformed-xref input all produce defined `Status` errors, never a crash; buffer-size
arithmetic cannot be made to overflow; a password containing an embedded NUL is rejected
outright rather than silently truncated at the NUL.

**What is still owed before this leaves bootstrap status** (see
`third_party/pdfium/README.md`): the hermetic from-source build SP-1 actually recommended, for
every real target platform including Android/iOS; resolution of the AGG-version residual risk
as a documented legal call, not just a flagged one; a fuzz target for the PDF parsing path
(spec §29 names the PDF parser first in its required-fuzz-targets list — `file-engine/
detection` has one, `pdf-engine/reader` does not yet); everything else in spec §11 beyond
viewing (edit, merge/split, forms, annotation, signatures, PDF/A, watermarks, redaction).

### Added — `fuzz_detect`: a real libFuzzer target over `file-engine/detection` (spec §29)

Closes the gate the `file-engine/detection` entry below left open (`testing-strategy.md` §6:
"a parser without a fuzz target does not merge"). It is the **first** fuzz target in the
repository, and it fuzzes the **buffer** entry point — `detect_buffer` and `describe_buffer` —
because that is the untrusted-upload shape: raw bytes, no path, no filesystem, no assumption
of well-formedness. It is what the mobile picker and the eventual HTTP upload handler call,
and the first first-party code that ever touches an attacker-supplied file.

- **`tests/fuzz/fuzz_file_detection.cpp`** — `LLVMFuzzerTestOneInput` feeds the input bytes in
  verbatim: no length prefix, no framing, no embedded control fields. That is a constraint,
  not a shortcut — it means every corpus entry is a real file, a crash reproducer is directly
  usable as a regression fixture, and nobody can later mistake a harness framing bug for a
  detector bug. The filename, the one thing file bytes cannot carry, is supplied from a fixed
  table indexed by the input's own first byte, so the advisory-hint ladder
  (`extension_of` → registry lookup → conflict resolution) is exercised deterministically
  without contaminating the corpus file format.
- **An oracle, not just a crash check.** ASan/UBSan catch memory errors; they do not catch a
  self-contradictory verdict, which for a detector is both likelier and still a security
  finding, since callers gate on `confidence` and `extension_conflicts`. Every input is
  additionally checked against the invariants `detector.h` states in prose — the filename can
  never set `format`, an identified format never carries `None` confidence, `magic_offset` is
  PDF-only and never past the input, container flags never appear on a non-ZIP result, two
  calls on identical bytes agree, `describe_buffer` never contradicts `detect_buffer`. **Only
  invariants the public header actually promises** are asserted; a stricter rule than the
  contract would manufacture fake defects.
- **`UAPP_ENABLE_FUZZING`, default OFF, a third build configuration rather than a flag on the
  normal one.** libFuzzer supplies `main`, so `-fsanitize=fuzzer` cannot go into the shared
  compiler posture without turning the CLI and all four test binaries into fuzzer drivers.
  What is applied project-wide under the switch is `-fsanitize=fuzzer-no-link,address,
  undefined` — the coverage instrumentation, with `main` left alone — and the fuzz target
  alone adds the runtime at link. Instrumenting the *library* and not just the harness is the
  point: a target built against an uninstrumented `uapp::file_detection` is a random byte
  generator with no feedback. With the switch ON, tests and tools default OFF and the SQLite
  probe is skipped, so a fuzz build produces the library under test and the harness and
  nothing else. No fuzz target is registered with ctest: it has no pass/fail, it has a budget.
- **A 24-file seed corpus** in `tests/fixtures/corpus/fuzz_file_detection/` (§6.2's location),
  holding a minimal but genuinely valid example of **each of the ten formats detection
  implements** plus the malformed shapes a mutator would take a long time to build — no
  central directory, a truncated one, an EOCD pointing past EOF, a Zip64 sentinel, a
  65534-entry claim, a bare `PK\x03\x04`, an unrecognised OOXML family, a PDF behind a
  300-byte preamble, an empty file. The ZIP-family seeds are byte-identical to what
  `tests/file_engine/zip_fixture.h` already builds, so fixture and corpus cannot drift apart.
- **A configure-time guard rail for macOS.** Apple's Command Line Tools clang accepts
  `-fsanitize=fuzzer` but ships no `libclang_rt.fuzzer_osx.a`. The configure step link-probes
  and fails with the `brew install llvm` remedy rather than letting the build reach the linker
  several hundred files later. This was not hypothetical — it is what this machine does.

**The run, actually performed** (LLVM 23.1.2, Debug, ASan+UBSan; full record in
`testing-strategy.md` §6.3): three bounded runs of 121 s each — 2 928 476, 2 166 152 and
2 247 966 executions at 24 202 / 17 902 / 18 578 exec/s — plus a 61 s re-run of the documented
smoke command verbatim against a clean rebuild (2 006 441 executions at 32 892 exec/s).
**9 349 035 executions over 424 s: zero crashes, zero hangs, zero sanitizer reports, zero
oracle violations.** Coverage settled at
907 edges / 1 877 features with the corpus plateaued at 154 units, so this is a *saturated
smoke run for this corpus and length cap* — not a claim that the module is clean, which needs
the ≥ 4 h nightly cadence §6.2 specifies. The third run raised libFuzzer's default 4096-byte
cap to 128 KiB, because the default never reaches the 8 KiB text-probe boundary or the 64 KiB
EOCD backward scan; it added 7 features and no defects.

**No product defect was found, and none is claimed.** That is a plausible result rather than a
surprising one: `ByteSpan` was built so that no accessor *can* read out of range and the ZIP
walk is capped and overflow-guarded, which is the property a first fuzz run is supposed to
confirm. The one bug the work did surface was in the **corpus generator**, not the product —
its central-directory record omitted the mod-date field, so the DOCX/XLSX/PPTX seeds were
silently degrading to plain `zip` and the fuzzer was being seeded with three fewer real
formats than intended. Caught by asserting each seed's detected format before the first run;
fixed in the generator, not in the detector. Recorded because a silently-wrong corpus is
exactly the failure mode that makes a fuzz run look fine while testing much less than it says.

**Verification.** The fuzz build configures and builds clean at `-Werror` under LLVM 23. The
normal build is untouched: a from-scratch `cmake -S . -B build_check && cmake --build
build_check && ctest` with `UAPP_ENABLE_FUZZING` at its default OFF gives **ctest 9/9** and the
same suite counts as before — notes 42, blocks 36, file_engine 65, capi 17 — with no
`-fsanitize` flag anywhere in `compile_commands.json`.

**NOT IMPLEMENTED**: the other seventeen targets in §6.1 (`fuzz_pdf_parse`, `fuzz_zip`,
`fuzz_xml`, `fuzz_sync_delta`, …). Each names a parser that is still an empty Phase 0
directory; a stub harness over a module that does not exist would turn the merge gate into a
checklist of targets that fuzz nothing. Also not done: the CI wiring for the §6.2 cadence
(60 s per PR, ≥ 4 h nightly, 72 h release gate) — there is no CI pipeline in this repo yet;
structure-aware mutation for the OOXML family; and MSan, which §6.2 requires to run separately
and which needs an instrumented libc++ this repo does not build.

### Added — `file-engine/detection`: real file-type detection (spec §8/§9)

The foundational answer to "what is this file, and what may we do with it?", which the PDF
engine, scanner, OCR and conversion will each call **before** trusting a filename. Detection
only: this module does not open, parse, render or convert anything, and must not — it is what
decides whether a parser should be invoked at all (`architecture.md` §5).

**Spec §8's "never trust the file extension alone" is enforced structurally, not by
convention.** `DetectionResult::format` is only ever assigned from bytes; there is no code
path by which a filename can set it. What the name claimed goes to `extension_hint`, and a
disagreement raises `extension_conflicts` — the security-interesting output, since an
`invoice.pdf` whose bytes are a ZIP is worth showing a human. The deliberate consequence: a
`.mp3` whose sniffer does not exist detects as `Unknown`, not as MP3. That is the honest
answer.

- **Detection implemented for ten formats**: PDF, PNG, JPEG, GIF, ZIP, DOCX, XLSX, PPTX, ODT,
  and UTF-8/ASCII plain text. Signatures are compared in full — all eight PNG bytes, not four,
  because the CRLF/EOF guard bytes exist precisely to catch the corruption a short check
  misses. PDF is scanned across the first 1 KiB rather than at offset 0 (real files carry
  preambles; every other viewer accepts them) and the **offset is reported** rather than
  silently normalised away.
- **The ambiguous case is the substance of the module.** DOCX, XLSX, PPTX, ODT, EPUB and an
  archive of holiday photos share the same four magic bytes. Two strategies, and which one was
  used is **reported** so a security decision is never taken on a guess: a **central-directory
  walk** (authoritative → `MagicAndContainer` confidence) and a **local-header walk** (best
  effort, defeatable by a crafted archive → capped at `Magic`, with `container_truncated` set).
  ODF is read from its `mimetype` member, which the ODF spec mandates be first and STORED for
  exactly this purpose. An OOXML package we cannot name stays `Zip` and sets
  `unrecognised_ooxml` — never a guess between the three families.
- **The API works on a path *and* on a buffer**, because a mobile picker returns a
  security-scoped URI or bytes, never a POSIX path. One engine (`detect_sample`, pure and
  `noexcept`) under three entry points; `detect_path` reads a bounded **head and tail** window
  and never the whole file — the tail exists solely because a ZIP's member list lives at the
  end, so a 10 MB DOCX is classified authoritatively from ~192 KB of reads.
- **The spec §8 `File` value** with all eleven fields. `location` is a tagged `FileLocation`,
  not a string: on mobile there is often no path, and a fake path is a path something will
  eventually try to open. `id` and timestamps are **passed in**, not generated — a detector
  that reaches for an ambient clock or RNG is untestable, and both are ports
  (`core/common/ports.h`).
- **Checksum: FNV-1a 64 with an avalanche finaliser**, tagged `ChecksumAlgorithm::Fnv1a64`.
  Chosen over SHA-256 because this field is an *integrity digest*, not a signature: libsodium
  is not vendored yet and §7.10 requires seven steps before any dependency lands, hand-rolling
  SHA-256 in the detection module is worse, and SHA-256 costs ~10× per byte over every byte of
  every file opened on Tier D hardware. Chosen over xxHash for the same §7.10 reason. The
  finaliser fixes FNV-1a's weak low-bit avalanche for three instructions *per file*.
  **`Checksum` is deliberately shaped as `{algorithm tag, length, 32-byte array}`, not a bare
  `uint64_t`** — adding SHA-256 later is then a new enumerator, not a data migration.
- **Capabilities are derived from `file-format-support.md`, not hand-written.** The derivation
  rules are now **§12 of that document**, and the notable results are enforced by tests:
  **DOCX gets no EDIT** (the matrix marks it P2), **PPTX gets EXTRACT and nothing else** (v1.0
  is text extraction for search), **nothing at all gets SIGN** (§5.1: "a half-implemented
  signature check is worse than none"), **only PDF gets ENCRYPT** (not ZIP — AES-ZIP is
  read-only), and **JPEG gets no COMPRESS** (re-encoding is lossy). An unidentified file gets
  the **empty** set, never a default of VIEW.
- **Malformed input is a first-class requirement (spec §28/§29), handled by the type system
  rather than by scattered length checks.** Every byte read goes through `ByteSpan`, which has
  no accessor that can read out of range: `equal_at` returns false rather than comparing a
  short buffer, `read_u32le` never half-writes its output, `subspan` clamps, and every
  `offset + length` is computed with an overflow guard so a `0xFFFFFFFF` length from a corrupt
  ZIP header cannot wrap. The ZIP walks are entry-capped, name-capped, must strictly advance,
  and reject a central directory whose offset lies outside the file rather than clamping it
  into live content.

**Deviation from `file-format-support.md`, recorded in its new §12.** Spec §8 names ten
capabilities; the matrix has six columns. **No existing cell was changed** — implementing
detection contradicted nothing — but EXPORT, COMPRESS, SIGN, ENCRYPT and EXTRACT had no column
and are now *defined* there (EXPORT ⇔ the CREATE column; COMPRESS ⇔ lossless repack where we
ship a writer; SIGN ⇔ nothing; ENCRYPT ⇔ PDF only; EXTRACT ⇔ the ZIP family and PDF).

**NOT IMPLEMENTED** (declared with a permanent enum value and a machine-readable reason, never
faked — the `block_type_not_implemented_reason()` precedent):

- **The other ~35 formats of spec §9.** The registry enumerates all of them, and answers three
  *different* questions: `format_detection_is_implemented(f)`, why not
  (`format_detection_not_implemented_reason(f)`), and — separately —
  `DetectionResult::format == Unknown`, meaning the sniffer ran and matched nothing. A test
  asserts the two queries can never disagree and that no unimplemented format is left without
  a reason. Summaries: legacy **DOC/XLS/PPT** share one OLE2/CFB header, so sniffing it names
  the container not the document, and §2 marks all three server-only anyway; **MD/HTML/XML/
  JSON/CSV/SVG/FB2/RTF** are text dialects with no magic number, so they detect as PlainText —
  true rather than wrong; **ODS/ODP/EPUB** need only a member-path signature added to the
  existing introspection; **HEIC/AVIF/MP4/MOV** share the ISO-BMFF `ftyp` brand space and
  deserve one sniffer rather than four half-sniffers; audio/video are deferred behind PQ-5 and
  the FFmpeg decision; **WEBP/TIFF/BMP/TAR/GZ/7Z** have trivial signatures but no engine
  behind them, and an unconsumed detection result is a claim with nothing behind it; **RAR** is
  permanently out of scope (§7.7).
- **Zip64 central-directory parsing.** The sentinels are *detected* and cause a documented
  fallback to the local-header walk, rather than following `0xFFFFFFFF` as a literal offset.
  Tested.
- **Legacy 8-bit charset detection** (Windows-1252, ISO-8859-x, Shift-JIS). A statistical
  problem needing ICU's `CharsetDetector` (§7.1 ADOPT, not integrated). Such a file fails the
  UTF-8 check and reports `Unknown` — "we cannot tell what encoding this is" beats handing
  mojibake to an editor.
- **Format-specific metadata** — EXIF/XMP, the PDF Info dictionary, OOXML `docProps`, ID3. All
  require opening the format. Putting an untrusted-input parser inside the module whose job is
  to decide whether a parser should run at all would invert `architecture.md` §5.
- **`DetectionHints::declared_mime_type` is accepted and ignored.** Declared now because the
  upload and Android-picker call sites are being written against this header, but using it
  needs a trust order between a host-asserted type and our own sniffing — a `security-model.md`
  policy question. Today the bytes win unconditionally, which is the safe default to be stuck
  with.
- **`FileLocation::Kind::PlatformUri` cannot be resolved**; that needs the platform vtable.
  Carried, not dereferenced.
- ~~**No fuzz target yet**~~ — **closed below**; `detect_buffer` now has one.

**One edit to `core/capi`, which this change was otherwise to leave alone** — the same
tripwire, and the same resolution, as the `core/blocks` change below. Adding the four 6xx
`Status` codes broke its *exhaustive* `to_c_status` switch under `-Werror=switch`. They are
listed explicitly and mapped to `UAPP_STATUS_UNEXPECTED`; **no new `UAPP_STATUS_*` constant was
minted** and no ABI surface changed, because `core/capi` still exposes only `NoteService` and
nothing reachable through that boundary can return one of them. That switch exists to force
this decision rather than allow a silent `default:`, so extending it is the mechanism working.

**Verification.** Configured and built from scratch in **Debug**, **Release** and
**Debug+ASan/UBSan**: **zero warnings under `-Werror`** in all three, with `-fno-exceptions`
and `-fno-rtti` unchanged. `ctest` **9/9 passed** in each. `uapp_file_engine_tests` reports
**65 passed, 0 failed**, including under ASan/UBSan. The new binary links **only**
`uapp::file_detection` → `uapp::core_common`, so "detection has no storage and no document
model behind it" is checked by the linker rather than by review.

**Two defects were found by these tests and fixed.** (1) The container/content exemption in
`resolve_conflict` was symmetric, so it suppressed the mismatch flag for *any* ZIP — meaning a
plain ZIP renamed `vacation.jpg` was reported without a conflict, defeating the one signal the
flag exists for. The rule is now asymmetric and evidence-aware: a `.docx` named `.zip` is not a
lie, a ZIP named `.jpg` is, and a ZIP named `.docx` is a conflict only when the directory walk
actually *proved* the `word/` part absent — a truncated container is unproven, not guilty.
(2) `central_directory_unreachable` was set whenever the directory scan produced no useful
evidence, conflating "we walked it and this is just a ZIP" with "we could not read it", which
would have lowered confidence on every ordinary archive.

### Added — `core/blocks`: the real block document model (spec §14)

The flat note is superseded. This implements the structure spec §14 actually asks for —
`Workspace -> Page -> Blocks`, with blocks nested inside blocks — as the model everything
later (backlinks, comments, mentions, block-level sync, version history) can anchor to. A
`body` string has nothing to anchor to; a block id does.

**Note vs. Page — the architectural call.** `Page` wins; `Note` is **deprecated in place, not
deleted**, and the two coexist for exactly as long as it takes to move `core/capi`. Reasoning
and the four-step retirement path are in **`docs/adr/0003-page-supersedes-note.md`**. The short
version: `core/capi` is built on `NoteService`, and the C ABI is the contract the Kotlin and
Swift bindings will be written against — churning it while the block model is one day old is
the wrong order. `Page` is also, today, *less* capable than `Note` in one respect (no
full-text search), so folding `Note` into it now would remove working functionality. `Note`
becomes a page holding one paragraph block once block FTS and the block ABI exist; no id
migration is needed, because `NoteId` was given the real 16-byte shape in the bootstrap slice.
`core/blocks` deliberately does **not** link `core/notes` — the build graph enforces that the
successor never acquires a dependency on what it replaces.

- **`core/blocks`** (`uapp::core_blocks`) — the portable domain. Links no database library, so
  PHASE0_SUMMARY.md H-3 stays enforced by the build graph rather than by convention.
  - **`Block` content is a `std::variant` over typed payload structs, not a class hierarchy.**
    Three reasons, recorded in `block.h`: a closed tagged union maps to C as
    `{int32_t type; union {...};}` mechanically, and ADR-0001 commits us to a C99 ABI; ADR-0001
    also bans RTTI, so a hierarchy would need a hand-rolled type tag anyway — a tagged union
    wearing a vtable; and the type set is closed and ours, which is exactly when `std::visit`'s
    compile-time exhaustiveness beats open polymorphism.
  - **Eight block types with real typed fields** (no opaque strings): `paragraph`, `heading`
    (level 1–3), `bulleted_list_item`, `numbered_list_item`, `checklist_item` (`checked` bool),
    `code` (`language` tag), `divider`, `quote`. Numbered-list numbering is *derived* from
    sibling position, never stored — storing it goes stale on every insert.
  - **The variant index IS the persisted type tag.** `static_assert`s pin each alternative to
    its `BlockType` value, so reordering the variant is a compile error instead of a silent
    reinterpretation of every stored block.
  - **`Page`** with id, title, timestamps and an ordered tree of blocks.
- **Nesting is real, and depth-capped at 64** (`kMaxBlockDepth`). Every read of a block tree is
  recursive, and the tree is user-controlled, so unbounded nesting is a stack-exhaustion DoS
  reachable from a restored or synced document — spec §38's "validate external input" applies
  to our own files too. The cap is enforced on **insert**, on **move** (destination depth +
  the moved *subtree's* height, not just the block) **and on read**, so a hand-edited database
  cannot smuggle a deeper tree past it. The two tree-measuring helpers are iterative with an
  explicit stack: the function that decides whether a tree is safe to recurse into must not
  recurse into it to find out.
- **`storage/sqlite/src/sqlite_block_store.cpp`** — the relational adapter. Pages and blocks in
  `page` + `block` tables with real columns and foreign keys, **not** a JSON blob: order,
  nesting and per-block identity have to survive into SQL for search, backlinks and block-level
  sync later. Same discipline as the note store — RAII `Connection`/`Statement`/`Transaction`,
  every mutation in one all-or-nothing transaction, `Status`-based propagation, no
  `<sqlite3.h>` in any public header, schema-version check that refuses a newer database.
  Deleting a subtree and re-densifying positions are one statement pair inside one transaction,
  so a half-applied move is unrepresentable.
- **Operations** (`DocumentService`): create page, open page (full tree), rename, delete page,
  add block at a position and nesting level, append, read block, update content (the type may
  change; children survive), delete block, reorder, move to a new parent. Sibling positions are
  **dense 0..n-1** after every operation, and an index past the end is `BlockPositionInvalid`
  rather than a silent clamp — a caller working from a stale tree should be told.
- **Delete cascades to children; it does not reparent them.** A nested bullet under a deleted
  bullet, or the body of a deleted toggle, is bound to its parent — promoting orphans into the
  grandparent produces a document the user did not write. It is also what
  `database-design.md` §3.3 already specifies (`ON DELETE CASCADE` on `block.parent_id`). Undo
  is the right answer to "I did not mean that", and is NOT IMPLEMENTED (below).
- **`core/common` gained `ObjectId<Tag>` and the generic `Clock`/`IdSource` ports.** The
  16-byte identifier and its hex parser now exist once instead of being re-hand-rolled per
  domain; the phantom tag keeps `PageId` and `BlockId` distinct types at zero runtime cost
  while staying a 16-byte standard-layout aggregate for the future C ABI. `uapp::Status` gained
  a generic `IdMalformed` (104) and an append-only 5xx block range.
- **`tests/`** — a **separate** `uapp_blocks_tests` binary (CTest: `uapp.blocks.unit`,
  `uapp.blocks.store`) that links `uapp::storage_sqlite` and never names `uapp::core_notes`, so
  ADR-0003's independence claim is checked by the linker. **36 cases**: create a page and fill
  it with every implemented payload; **persist and reload across a real close/reopen, asserting
  order and nesting exactly** (three top-level blocks, two children, a grandchild, parent links
  round-tripped); positional insert; update content and change a block's type with children
  intact; **delete cascading to a whole subtree** plus the sibling gap closing; delete page;
  reorder (front, back, middle-to-middle); **move to a new parent with the subtree travelling
  with it**, and back out to the top level; cycle rejection (under a descendant, and under
  itself); **the depth guard accepting exactly 64 and rejecting the 65th**, with nothing written
  by the failed attempt; a move refused because the *subtree* would not fit where a lone block
  would; and error paths — block on a nonexistent page, nil ids, parent on another page,
  position past the end, unknown block id, read-only database, entropy-source failure. Plus the
  domain-only suite: id hex round-trip and rejection, the type table, payload validation
  (heading level, embedded NUL, oversize, code-language control bytes) and tree validation.

**Two real defects were found by these tests and fixed**, both recorded because neither is
visible by inspection. (1) `Statement::bind_text` in the shared `sqlite_raii.h` bound **SQL
NULL** for any empty `std::string_view`, because a default-constructed view has
`data() == nullptr` and `sqlite3_bind_text` reads a null pointer as NULL. Against a `NOT NULL`
column that surfaced as `SQLITE_CONSTRAINT` — reported as "already exists" for a row that did
not exist. This was latent in the note store too. (2) The tree reassembler rejected a tree at
*exactly* the depth limit, because it judged depth before checking whether the deepest block
had any children at all.

**Deliberate deviations from `database-design.md` §3.3**, recorded so they are not mistaken for
oversights, both with their migration written down at the point of deviation:

- **Ordering is an integer `sort_index`, not a fractional `sort_key`.** The design doc is right
  that integer positions turn a 1-block insert into an N-row renumber, which becomes an N-block
  *conflict* once sync exists. That cost is only paid once sync exists (Phase 13); today there
  is one writer on one device, the renumber is the tail of one sibling list inside the same
  transaction, and dense positions make every ordering assertion exact instead of a comparison
  of two opaque strings. Deferred, not rejected: the migration is one `ALTER TABLE` plus a
  single backfill pass in `sort_index` order, with no change to any identifier.
- **Block content is in four typed columns, not a CBOR blob.** CBOR is the right answer for
  nineteen types with disjoint fields; with the eight implemented here the whole payload space
  is `text`/`checked`/`heading_level`/`language`. A hand-rolled codec would need its own fuzz
  target before it could be trusted with a file off the network. `schema_ver` is present from
  day one, and `block.h` names the trigger for the switch: the first type (`table`, `image`, …)
  whose payload does not fit these columns.

**NOT IMPLEMENTED** (declared, reasoned, and not faked):

- **Eleven of the nineteen §14 block types.** `toggle`, `callout`, `table`, `image`, `video`,
  `audio`, `file`, `equation`, `embed`, `database`, `bookmark` are enumerated in `BlockType`
  with permanent numeric values and each returns a machine-readable reason from
  `block_type_not_implemented_reason()` — in code, not in a comment, so the reason cannot rot
  and a UI can show it. Summaries: `table`/`database` need the §15 property and view system
  (explicitly a later run); `image`/`video`/`audio`/`file` need the blob store, since
  database-design.md principle 6 forbids putting bytes in the database; `embed`/`bookmark`
  fetch remote URLs, which breaks offline-first and needs the network policy gate and URL
  allow-listing; `equation` needs a LaTeX subset parser that deserves its own fuzz target;
  `toggle` needs per-viewer collapsed/expanded view state (its child-block half is already
  covered by generic nesting); `callout` needs the icon and colour-token vocabulary.
  Constructing one is impossible — there is no variant alternative — and reading one written by
  a newer build yields `BlockTypeUnsupported` rather than a silently-wrong empty paragraph.
- **Undo/redo**, and therefore **version history**. Out of scope for this run by instruction,
  and deliberately *not* a stub method: doing it properly means an operation log with inverses,
  which changes how every mutation is recorded. Bolting it on later is cheaper than faking it
  now.
- **Copy/paste** (needs a clipboard serialisation format), **backlinks** and **mentions** (need
  inline rich-text spans plus a link index), **comments**, **sharing**, **permissions**,
  **templates**.
- **Rich text.** A block's text is a flat UTF-8 string. Inline bold/italic/link spans need an
  inline run model plus the sequence CRDT that `database-design.md` reserves `block.crdt_state`
  for.
- **Child pages, databases, files and workspaces** — the other three arms of the §14 tree.
  `page` has no `parent_id` or `workspace_id` column: absent rather than present-and-ignored,
  so nothing can half-work. All three are additive migrations.
- **Full-text search over block text.** No counterpart to `note_fts` yet; it needs the
  denormalised `text_cache` fed into FTS5 under the same one-transaction discipline. This is
  the one respect in which `Page` is currently *less* capable than `Note`, and it is step 1 of
  ADR-0003's retirement path.
- **Cross-page block moves** — rejected with `InvalidArgument`. The whole subtree's `page_id`
  would have to be rewritten, and once backlinks and per-page FTS exist those indexes move with
  it. A half-moved subtree is a corrupt document, so refusing is the honest answer.
- **UUIDv7 layout.** Identifiers are 16 random bytes, so they lack the B-tree locality v7 buys.
  The shape is already right, so this is a one-file change with no format migration.

**One edit to `core/capi`, which this change was otherwise to leave alone.** Adding Status
codes broke its *exhaustive* `to_c_status` switch under `-Werror=switch`. The nine new codes
are listed explicitly and mapped to `UAPP_STATUS_UNEXPECTED`; no new `UAPP_STATUS_*` constant
was minted. The C ABI is separately versioned and its codes are permanent, and nothing
reachable through that boundary can return one of these yet — `core/capi` still exposes only
`NoteService`. No ABI surface was added or changed.

**Verification.** Built from scratch in Debug, Release, and Debug+ASan/UBSan: **zero warnings
under `-Werror` in all three**, and `ctest` **6/6 passed** in each. `uapp_blocks_tests` reports
**36 passed, 0 failed**; the pre-existing suites are unchanged — `uapp_notes_tests` still
**42 passed, 0 failed**, the C ABI binary still **17 passed, 0 failed**, and
`uapp.notes.cli_demo` still passes.

### Added — Phase 1 bootstrap: the note vertical slice (first production code)

Implements exactly the slice spec §40 names as the task after architecture approval:

```text
CREATE NOTE -> STORE LOCALLY -> SEARCH NOTE -> OPEN NOTE -> EDIT NOTE -> SAVE NOTE
```

Nothing else from Phase 1 is done. This is the seed, not the phase.

- **Buildable CMake project at the repo root.** C++17 per ADR-0001. Top-level
  `CMakeLists.txt` plus `cmake/UappCompilerOptions.cmake`, which encodes the ADR's
  restricted subset as toolchain flags — `-fno-exceptions -fno-rtti`, `-Wall -Wextra
  -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Werror`. Options:
  `UAPP_BUILD_TESTS`, `UAPP_BUILD_TOOLS`, `UAPP_ENABLE_SANITIZERS`, `UAPP_WARNINGS_AS_ERRORS`.
- **`cmake/FindSQLite3Fts5.cmake`** — locates SQLite *and compiles-and-runs a probe* proving
  FTS5 is present, failing configuration otherwise. An FTS5-less libsqlite3 would not fail at
  link time; it would fail on a user's device the first time anyone searched.
- **`core/common`** (`uapp::core_common`) — `uapp::Status`, a flat append-only error enum with
  both a stable machine-readable code (`NOTE_NOT_FOUND`) and a human-readable message, per §33.
  Every fallible call in the core returns it and is `[[nodiscard]]`.
- **`core/notes`** (`uapp::core_notes`) — the portable domain. `NoteId` (16-byte opaque id,
  validating hex parse), `Note`, validation with hard size limits, the `NoteStore` persistence
  **port**, `Clock`/`IdGenerator` ports, and `NoteService` — the public API implementing the
  six slice steps. Links no database library at all, so PHASE0_SUMMARY.md H-3 ("nothing outside
  `storage/sqlite` includes `sqlite3.h`") is enforced by the build graph, not by convention.
- **`storage/sqlite`** (`uapp::storage_sqlite`) — the SQLite adapter. RAII wrappers
  (`Connection`/`Statement`/`Transaction`, rollback-on-destruction) so no early return can leak
  a statement or double-free one; WAL + `foreign_keys=ON` + busy timeout; bind helpers that
  check every `size_t`→`int` narrowing before it reaches the C API; content and FTS5 index
  written in **one transaction** per database-design.md §3.9; schema-version check that refuses
  a database written by a newer build.
- **`storage/sqlite/fts_query`** — converts raw user input into a safe FTS5 `MATCH` expression
  via an allow-list (word bytes only, each term double-quoted, ANDed, last term prefix-matched).
  Binding user text straight to `MATCH` is not SQL injection but *is* injection into FTS5's
  query language; this is where §38's "validate external input" applies to search.
- **`tools/notes_cli`** (`uapp-notes`) — developer harness. `demo` drives the whole slice
  against a real on-disk database and verifies every step, exiting non-zero on the first
  failure; `create`/`search`/`open`/`edit`/`delete`/`count` allow manual use.
- **`tests/`** — 42 unit/integration cases plus a CLI integration test, wired into CTest as
  `uapp.notes.unit` and `uapp.notes.cli_demo`. Coverage includes create, persist-and-reload
  across a closed connection, FTS hit *and* miss, prefix match, edit-then-reload, index
  freshness after edit and delete, UTF-8 round-trip, and eleven error-path cases (unknown id,
  nil id, save-after-delete, oversized title/body/query, read-only database write rejection,
  unopenable path, empty path, entropy-source failure, duplicate id, hostile search input).

**Verification.** Debug, Release, and ASan+UBSan configurations all build with zero warnings
under `-Werror`; `ctest` is 2/2 and the unit binary reports **42 passed, 0 failed** in each.

### Added — the C ABI (`libuapp_capi`): the platform-bridge boundary

Infrastructure, not a feature. The note slice already worked and was tested; this gives it a
C-callable face so the Android (JNI) and iOS (Swift/ObjC) bindings have something to attach to.
No note behaviour changed — `core/notes` and `storage/sqlite` are untouched.

Implements PHASE0_SUMMARY.md **H-2** (split the spec §4 "platform bridge" into a versioned C
ABI plus hand-written idiomatic bindings, so a C++ ABI never crosses JNI or Swift interop) and
the ABI rules in `api-design.md` §10 / `mobile-architecture.md` §2.

- **`core/capi`** (`uapp::capi`) — two pure C99 public headers. `uapp/capi/uapp_capi.h` carries
  the ABI version, the status enum and the string-ownership rules; `uapp/capi/uapp_notes.h`
  carries the note surface. Opaque handles only (`uapp_note_service_t*`, `uapp_note_t*`,
  `uapp_search_results_t*`); no C++ type, no STL container and no C++ ABI crosses the line. The
  implementation is C++17 under the same `-fno-exceptions -fno-rtti -Werror` posture as
  everything else. It is also the composition root that binds the abstract `NoteStore` port to
  the SQLite adapter — a PRIVATE link, so nothing above it inherits a path to `sqlite3.h`.
- **Versioned per spec §30.** `UAPP_ABI_VERSION_MAJOR/MINOR/STRING` plus real exported
  accessors and `uapp_abi_is_compatible()`, so a binding can interrogate the library it
  actually loaded rather than the header it compiled against. Structs crossing the boundary
  carry a leading `uint32_t struct_size`; a caller with a smaller struct is rejected, one with
  a larger struct is served. **Major is 0 deliberately** — the machinery is complete and
  enforced, the surface is not yet frozen. The freeze and the ABI-diff CI job over the exported
  symbol list (`api-design.md` §10 rule 5) land with the first shipped release.
- **Every fallible function returns `uapp_status_t`**, a C mirror of `uapp::Status` whose
  numeric values are asserted identical to the C++ enum's — code by code, at compile time — so
  a renumbering in the core is a build failure rather than a shipped client reporting the wrong
  error a release later. Two C-ABI-only codes in a 9xx block: `OUT_OF_MEMORY` and `UNEXPECTED`.
  The only `void` functions are the destructors and the pure version/string lookups.
- **Nothing unwinds across the boundary.** Every exported body is wrapped in a guard that
  converts an escaping exception into `UAPP_STATUS_UNEXPECTED`. Under the shipping
  `-fno-exceptions` build it compiles to nothing; it exists because the flag is per translation
  unit, and a future engine linked in with exceptions on must not unwind through a JNI frame.
- **String ownership is a two-rule contract, and the function names carry it.** `uapp_*_get_*`
  borrows from the handle (freeing it is a bug; it dies with the handle). `uapp_*_copy_*`
  transfers ownership and **must** be released with `uapp_free_string()`, never `free()` — the
  host and the core need not share an allocator. Status and version strings are static storage.
- **Handle hygiene.** Every destructor takes a *pointer to* the handle and NULLs it, so
  double-release is a no-op and the ordinary use-after-close mistake degrades to
  `INVALID_ARGUMENT` instead of a native crash. Each handle type carries a distinct magic
  cookie, so a NULL, zeroed or **wrong-type** handle is rejected rather than dereferenced. This
  is defence in depth, not a memory-safety claim, and the header says so.
- **No hidden global state.** No `uapp_ctx_t` singleton, no thread-local last-error slot, no
  process-wide registry. A service handle owns its connection, clock and id generator; any
  number may coexist and they share nothing. Proved, not asserted — see the tests.
- **`tests/capi/note_capi_test.c`** — **a C file**, compiled by the C compiler at `-std=c99`
  with `-Wall -Wextra -Wpedantic -Wconversion -Werror`, including only the two public headers
  and linking only `uapp::capi`. It is simultaneously the functional test and the
  C99-conformance check `architecture.md` §8 rule 5 asks for: a C++-ism in a public header
  breaks the build. 17 cases, wired into CTest as `uapp.capi.slice` and `uapp.capi.boundary`.
  Coverage: the full slice (create/store/search/open/edit/save/delete/count), persistence
  across close-and-reopen, UTF-8 round trip, search-limit default and clamp, **two simultaneous
  independent services proving isolation in both directions**, and boundary safety — null
  handles on every entry point, null strings, null out-parameters, use-after-close,
  double-close, wrong-type handles, malformed ids, oversized input, out-of-range result
  indices, `struct_size` versioning, and an unopenable database.
- **Build.** The project now enables C as a first-class language (`CMAKE_C_STANDARD 99`,
  extensions off). `cmake/UappCompilerOptions.cmake` and `core/common/CMakeLists.txt` gate the
  C++-only flags (`-fno-exceptions`, `-fno-rtti`, `-Wold-style-cast`, …) on
  `COMPILE_LANGUAGE:CXX`, so one options target still governs both languages and the warning
  posture is identical for each.

**Deliberate deviations from the `api-design.md` §10 sketch**, recorded so they are not mistaken
for oversights. That sketch describes the eventual full ABI; this is the note slice only. No
`uapp_init`/`uapp_ctx_t` — a process-wide context is exactly the hidden global state a
multi-handle design must avoid, and nothing here needs one. No `uapp_last_error()` TLS slot —
errors are self-describing codes with static message strings, which suffices until the
`uapp_error_t` cause chain exists. No FlatBuffers payloads — a flat title+body note has no
structured payload to carry; they arrive with the block tree. No job/progress/cancellation
protocol — every operation here is a local SQLite call and none can block indefinitely, so it
would be scaffolding around nothing.

**Verification.** Built from scratch in Debug, Release, and Debug+ASan/UBSan: **zero warnings
under `-Werror` in all three**, and `ctest` **4/4 passed** in each. The C ABI binary reports
**17 passed, 0 failed**; the pre-existing C++ suite still reports **42 passed, 0 failed**,
unchanged. The public headers additionally compile clean standalone under
`-std=c99 -pedantic-errors`, `-std=c11` and `-std=c17`, and as C++11/C++17 with
`-Wold-style-cast -fno-exceptions -fno-rtti` (the posture an ObjC++ iOS bridge would use).
macOS `leaks` reports **0 leaks** for the C ABI test binary.

Two defects were found by tooling during this work and fixed; both are recorded because neither
is visible by inspection. **UBSan** caught that reading an out-of-range `uapp_status_t` is
undefined behaviour — and a C caller can pass any `int` — fixed by pinning the enum with a
`UAPP_STATUS_FORCE_INT32` sentinel, which also guarantees the type is 32 bits wide on every
toolchain instead of implementation-defined. **`leaks`** caught that nulling an out-parameter on
a failure path silently discards whatever handle the caller's variable was holding; the ABI now
matches `NoteService`'s own contract — **out-parameters are never written on failure** — and a
test asserts exactly that.

### Test framework decision

No third-party test framework. Catch2 and GoogleTest both need exceptions, which ADR-0001
disables core-wide — adopting either would mean testing a configuration we do not ship.
`FetchContent` would also make `cmake` require the network, a poor default for an offline-first
product, and §35 requires a real license/redistribution review plus a `THIRD_PARTY_NOTICES`
entry before any dependency lands. `tests/support/test_framework.h` is ~120 lines providing
named cases, a registry, fatal/non-fatal assertions, a name filter and a pass/fail count.
Recorded as a bootstrap decision to revisit (with an ADR) when the suite needs fixtures or
CI-consumable output.

The C ABI test carries its own ~40-line harness for a further reason: it is a **C** file, and
`tests/support/test_framework.h` is C++. Including it would defeat the point of the file, which
is that only the public C headers are reachable from it. The two harnesses print the same
format on purpose, so the output of the two suites reads identically.

### Known gaps — deliberately out of scope for this slice, recorded so they are not forgotten

- **NOT IMPLEMENTED: end-to-end encryption.** ADR-0002 decision 1 makes E2EE a **v1.0
  requirement**. This slice stores note titles and bodies as plaintext in an unencrypted SQLite
  file. SQLCipher integration and the key hierarchy in `security-model.md` must land before any
  build carries real user data.
- **NOT IMPLEMENTED: sync, CRDT, multi-device.** Single-device only. The Fugue CRDT is Phase 13
  and needs its own correctness-proof spike first (ADR-0002 amendment); no CRDT work was started.
- **Partially implemented: the C ABI (`libuapp_capi`).** *(Was: NOT IMPLEMENTED, then
  narrowed to "the supported boundary for the note slice". At ABI 0.2 it is the supported
  boundary for the note slice, the page/block document model AND unified search, all three
  tested from C.)* What is **not** covered, precisely:
  - **~~Notes only.~~** *(Narrowed by the ABI 0.2 entry at the top of \[Unreleased]. Was:
    "`uapp_note_*` is the entire surface. There is no C ABI for documents, blocks, files, PDF,
    OCR, scanner, search-beyond-notes, sync or AI, because none of those engines exist.")*
    **Notes, pages/blocks and unified search are now covered**, by `uapp_notes.h`,
    `uapp_blocks.h` and `uapp_search.h` respectively. What remains **NOT IMPLEMENTED**, stated
    exactly rather than as a list of everything that is not notes:
    - **No file / detection ABI.** `file-engine/detection` exists and is tested in C++, but has
      no C surface. Deliberate: that engine's `Status` codes are still mapped to
      `UAPP_STATUS_UNEXPECTED` in `to_c_status`, precisely because nothing reachable through
      the boundary can produce one.
    - **No PDF ABI.** Same position, same reason — and `pdf-engine/reader` is the subsystem
      most likely to want a rich ABI vocabulary ("wrong password" must read differently from
      "this file is damaged"), which is exactly why no `UAPP_STATUS_PDF_*` constant has been
      minted on a guess.
    - **No OCR, scanner, sync or AI ABI.** None of those engines exists at all, so there is
      nothing to wrap.
    - **No child pages, databases, files-on-a-page, or the eleven declared-but-unbuilt block
      types.** `uapp_blocks.h` exposes exactly the eight implemented types and reports the
      other eleven as `UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED` with a machine-readable reason.
      These are core gaps (`core/blocks`), not ABI gaps: the ABI covers what exists.
    - **No undo/redo, version history, backlinks, mentions, comments, sharing, permissions or
      templates.** Also core gaps, recorded under the `core/blocks` entry below.
  - **NOT IMPLEMENTED: the `uapp_error_t` cause chain** from architecture.md §7. The boundary
    carries a code plus a static, content-free message — no `domain`, no `detail`, no causes.
  - **NOT IMPLEMENTED: the job protocol** (`uapp_job_submit`/`cancel`, progress and done
    callbacks) and the `uapp_platform_vtable_t` from mobile-architecture.md §6. Nothing in this
    slice is long-running or needs a platform service, so neither would have been exercised.
  - **NOT IMPLEMENTED: FlatBuffers payloads** (`uapp_buf_t`) for structured data, per
    api-design.md §10 rule 4. Search results cross as an opaque handle with typed accessors,
    which is adequate for four scalar fields and would not be for a block tree. *(Still true
    at ABI 0.2, and now load-bearing: the block tree crosses as a flattened pre-order view
    behind a handle rather than as a buffer. The argument for that choice, and the point at
    which a buffer becomes the right answer instead, is in `uapp_blocks.h`.)*
  - **Deferred: shared-library packaging and the ABI freeze.** `libuapp_capi` is a static
    archive; the `.so`/`.framework` build with a closed export set (the `UAPP_CAPI_API`
    visibility annotations are already in place for it) is not done, and neither is the
    ABI-diff CI job over the exported symbol list required by api-design.md §10 rule 5. The
    version is **0.1** precisely because the surface is not frozen yet.
  - **Deferred: the bindings themselves.** No `uapp-kotlin`, no `UAPPKit`, no JNI or Swift
    code, no Gradle or Xcode project. This run built the boundary those will attach to and
    stopped there.
  - **Not proven: concurrent use from multiple threads.** Distinct handles share no mutable
    state and are designed to be independent, and the tests prove independence — but they
    exercise it sequentially on one thread. "Safe to use two handles from two threads at once"
    is a design property here, not a tested one.
- **Deferred: vendoring SQLite.** The build links the platform libsqlite3. Reproducible mobile
  builds need the vendored amalgamation (+ SQLCipher).
- **Deferred: external-content FTS5 and an ICU tokenizer.** `note_fts` stores its own copy of
  the text, and `unicode61` does not segment CJK — both are Phase 9 items already named in
  database-design.md §3.9.
- **Deferred: the real document/block schema.** `note` is a flat title+body table, not the
  `document`+`block` tree of database-design.md §3.3. The 16-byte BLOB id matches the real
  design so growing into it needs no id migration.
- **Deferred: allocation-failure handling in C++ containers.** *(Narrowed — the C ABI boundary
  now exists and honours ADR-0001's contract for its own allocations: handles and owned strings
  use `new (std::nothrow)`, check, and return `UAPP_STATUS_OUT_OF_MEMORY` having changed
  nothing.)* What remains: `std::string`/`std::vector` growth **inside** the C++ core and
  inside the handle structs still terminates on OOM under `-fno-exceptions`. So the boundary
  reports allocation failure it can see and aborts on allocation failure it cannot. Making that
  contract complete means a nothrow-aware container policy through `core/`, which is its own
  piece of work.
- **Deferred: fuzz targets.** §29 and architecture.md §5 require a `LLVMFuzzerTestOneInput`
  entry point per parser. `build_fts5_match_expression` is the natural first target and has
  unit tests but no fuzz harness.

---

## [0.0.0] — 2026-09-29 — Phase 0: architecture and feasibility study

**No production code.** This release is documentation only, per spec §40
(*"Do not write production code until this analysis is complete"*).

### Added

- `docs/product-requirements.md` — PRD: personas, prioritised feature inventory (P0–P3),
  user stories with acceptance criteria, measurable quality attributes, explicit non-goals,
  v1.0 release gates, and 7 open product questions requiring sign-off.
- `docs/architecture.md` — system architecture. Five whole-system alternatives evaluated
  (cross-platform UI framework, Rust core, thin client, microkernel/process isolation,
  the spec's own layering). The spec §4 diagram is **accepted with three revisions**: split the
  platform bridge into a C ABI plus idiomatic bindings; collapse the duplicated persistence
  layers into one port; add an explicit Policy & Audit layer and a diagnostics spine.
- `docs/technical-requirements.md` — module architecture, port contracts, a technology
  selection matrix with rejected alternatives and confidence levels, six known-hard
  sub-problems, and 8 time-boxed Phase 0 spikes (~32 engineer-days).
- `docs/database-design.md` — SQLite local schema across 13 namespaces with sync columns,
  UUIDv7 identifiers, fractional indexing, EAV for user-defined databases (with the rationale
  and a documented escape hatch), plus the PostgreSQL cloud schema and the local↔cloud
  mapping. Capacity assumptions stated so the design is falsifiable.
- `docs/api-design.md` — REST surface refining spec §31 (deviations marked and justified),
  path-major versioning with a 24-month support window, idempotency, job protocol, cursor
  pagination, error envelope, rate limits, auth model, and the separately-versioned core C ABI.
- `docs/security-model.md` — assets, four trust boundaries, a STRIDE threat model with ~35
  enumerated threats and mitigations, key hierarchy with chunked AEAD, authorisation model,
  a correct redaction procedure, and **seven explicitly accepted risks**.
- `docs/ai-architecture.md` — provider abstraction (primitives vs. task services), a
  non-bypassable policy gate with an unforgeable policy token, router, local inference with
  honest limits, an 8-stage RAG pipeline including **citation verification**, agent framework
  with mechanically-enforced §20 prohibitions, model licensing rules, and an eval harness.
- `docs/file-format-support.md` — the full support matrix over all spec §9 formats with the
  required columns, plus a **dependency and licence analysis**. Key findings in §7 below.
- `docs/conversion-architecture.md` — three intermediate representations (UDM, SSM, IMM),
  a three-tier conversion strategy (direct / IR / render), a declarative capability graph,
  the full conversion matrix with fidelity classes, the `Unsupported` node contract, and
  automated post-conversion verification.
- `docs/sync-architecture.md` — per-datatype merge strategies rather than one algorithm,
  version vectors for exact conflict detection, Lamport clocks (**wall clocks never used for
  ordering**), outbox pattern, push/pull protocol, and a mechanical statement of the
  "never silently overwrite" invariant.
- `docs/offline-architecture.md` — the offline capability matrix including what does *not*
  work, the three-state connectivity model, on-demand asset handling, storage eviction with
  unsynced data never evicted, checkpointing instead of background-time assumptions, and the
  required UI states.
- `docs/mobile-architecture.md` — Android (Kotlin + Compose + JNI) and iOS (Swift + ObjC++
  bridge + SwiftUI/UIKit) architectures, JNI lifetime and threading rules, copy-free bitmap
  paths on both platforms, and the platform-service vtable.
- `docs/testing-strategy.md` — test levels weighted by damage class, property-based testing,
  mandatory fuzzing per parser with 18 named targets, the full fixture inventory from spec §28,
  the CI pipeline, and the **format-matrix verification test** that makes the support matrix
  unable to drift from reality.
- `docs/performance-strategy.md` — reference device tiers and ~60 measurable budgets,
  techniques mapped to where they are load-bearing, thermal/power policy, the benchmark
  regression gate, and 8 known performance risks.
- `docs/deployment.md` — monorepo build pipeline for six artefact families, four independent
  version axes, release trains, staged rollout with automatic halt criteria, kill switches
  (the only rollback mobile has), and backend deploy/migration practice.
- `docs/contributing.md` — development workflow, the non-negotiable rules from spec §37/§38,
  code standards, the nine-item dependency checklist, and the Definition of Done.
- `docs/roadmap.md` — the 20 phases from spec §36 expanded with entry criteria, deliverables,
  and phase-specific "done when" lists tied to the spec §41 Definition of Done; plus sequencing
  constraints and the phases most likely to slip.
- `docs/adr/0001-core-language-c-vs-cpp.md` — ADR: **C++17 internals behind a C99 ABI**, with
  four alternatives evaluated and a disciplined C++ subset defined.
- `docs/risk-register.md` — 28 risks with probability, impact, owner role, mitigation, and
  trigger conditions.
- `docs/engineering-complexity-estimate.md` — phase-by-phase effort estimate with complexity
  drivers, confidence levels, and the historical basis for the estimate.
- `THIRD_PARTY_NOTICES` (repo root) — initialised from the licence findings.
- `docs/PHASE0_SUMMARY.md` — decisions made with high confidence, decisions needing human
  sign-off before Phase 1, and spec ambiguities resolved by judgment call.

### Key licence findings (§7 of `file-format-support.md`)

- **PDFium (BSD-3-Clause) + QPDF (Apache-2.0)** selected for PDF. Both are safe for closed
  commercial distribution and static linking.
- **MuPDF (AGPL-3.0), Poppler (GPL-2.0+), Xpdf (GPL)** — **BLOCKED** for closed distribution.
  MuPDF remains available under a paid Artifex commercial licence if the business chooses.
- **LGPL-3.0 is a hard block for the iOS client.** Its anti-tivoization/relinking requirement
  cannot be satisfied under App Store code signing. This blocks **libheif, libde265, libmobi**.
- **HEIC/HEIF** is additionally blocked by active HEVC patent-pool exposure. Decision: use
  platform decoders on mobile only; **HEIC is unsupported on desktop and web**. AVIF
  (royalty-free) is the strategic alternative.
- **RAR is permanently out of scope** — the `unRAR` licence forbids the use we would need.
- **Legacy binary DOC/XLS/PPT** have no viable permissively-licensed C/C++ parser (antiword,
  catdoc, wv are all GPL). Marked server-only via headless LibreOffice (MPL-2.0, separate
  process), and therefore **not available offline**.
- **FFmpeg deferred.** LGPL static-linking friction plus H.264/HEVC/AAC patent exposure.
  v1.0 ships no transcoding; playback uses the platform media stack.
- **PDFium's vendored AGG copy requires explicit verification** at the pinned commit — AGG 2.4
  and earlier are permissive, AGG 2.5 is GPL.
- **FreeType must be elected under the FTL**, not its GPLv2 alternative.
- **Redis ≥ 7.4 (RSALv2/SSPL)** avoided server-side in favour of Valkey (BSD-3).

### Notes

- No source code exists in `core/`, `file-engine/`, `pdf-engine/`, `scanner/`, `ocr/`,
  `media/`, `ai/`, `knowledge/`, `storage/`, `network/`, `security/`, `api/`, `mobile/`,
  `desktop/`, or `web/`. This is intentional and required by spec §40.
- Every capability in every matrix in these documents is a **plan**, not a status. The v1.0
  release gate requires automated proof of each claim before it may stand.
- Licence findings were compiled from upstream licence files and public documentation and
  **must be re-verified against the pinned upstream revision at integration time, and reviewed
  by counsel** before Phase 5 and Phase 7 begin.

[Unreleased]: #unreleased
[0.0.0]: #000--2026-09-29--phase-0-architecture-and-feasibility-study
