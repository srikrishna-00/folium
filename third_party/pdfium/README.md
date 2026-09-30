# `third_party/pdfium` — a **bootstrap** PDFium, not the shipping one

> **This directory does not contain what SP-1 recommended.** It contains a
> prebuilt binary downloaded from a third-party distributor, pinned by tag and
> SHA-256, so that `pdf-engine/reader` can be written and tested today. The
> hermetic from-source build is still owed before anything ships.
> See `docs/spikes/SP-1-findings.md` §5 and the "What is still owed" section
> below.

## What is here

| Path | Committed? | What it is |
|---|---|---|
| `VERSION` | yes | The pin: tag, version, URL, SHA-256, the fetched `args.gn`, and the licence verification. The audit trail. |
| `fetch_pdfium.sh` | yes | Downloads, **verifies the SHA-256**, extracts. Refuses on mismatch. |
| `licenses/` | yes | Licence texts copied out of the fetched archive by the script. |
| `args.gn.<platform>` | yes | The exact GN args the fetched binary was built with. |
| `<platform>/` (e.g. `mac-arm64/`) | **no — gitignored** | The extracted payload: `include/`, `lib/libpdfium.dylib`, `licenses/`. |

## Why the binary is fetched rather than committed

- **Size.** `pdfium-mac-arm64.tgz` is 3.5 MB compressed; the extracted
  `libpdfium.dylib` is 7.3 MB. The full platform matrix this project actually
  needs (android arm/arm64, ios device + simulator, mac, and a CI linux) is
  roughly 40 MB of binaries that change every time upstream rolls. Git stores
  every historical copy forever, and there is no LFS configured here.
- **The root `.gitignore` already excludes `*.dylib` and `*.a`,** so
  committing the payload would mean carving an exception into a rule the repo
  set deliberately.
- **It is the wrong artefact to preserve.** SP-1 says this binary gets
  replaced by one we build ourselves. Committing it makes that replacement
  look like a regression in the history rather than the intended arc.
- What *is* committed — the tag, the SHA-256, the URL, the `args.gn`, the
  licence texts — is everything needed to reproduce and audit the fetch. That
  is the property that matters, and it is the property `THIRD_PARTY_NOTICES`
  rule 2 asks for (pinned revision + SHA-256 + LICENSE + a notices entry).

## Why the fetch is not a CMake `file(DOWNLOAD)` / `FetchContent`

`cmake -S . -B build` must work offline. `tests/support/test_framework.h`
already states this as the reason for not adopting Catch2, and it is a
stronger rule for a product whose first architectural principle is
offline-first. So **CMake never touches the network.** It only looks at the
filesystem, and `UAPP_ENABLE_PDF` defaults to whatever it finds. The network
step is one explicit command, run once, by a human or by CI.

## Setup

```sh
third_party/pdfium/fetch_pdfium.sh          # infers the host platform
cmake -S . -B build -DUAPP_ENABLE_PDF=ON
cmake --build build -j
ctest --test-dir build -R uapp.pdf --output-on-failure
```

Without the fetch, `cmake -S . -B build` still configures and builds
everything else; `UAPP_ENABLE_PDF` simply comes out OFF and `pdf-engine/` is
skipped. Passing `-DUAPP_ENABLE_PDF=ON` with nothing vendored is a hard
configure error with the fetch command in the message — an explicit request
should never be silently downgraded.

## What is still owed (do not let this directory become "PDF: done")

SP-1's actual recommendation is: build PDFium from a pinned source commit with
upstream's own GN/depot_tools toolchain, inside a hermetic container, in our
CI, and consume the result as an IMPORTED target. Against that, this
bootstrap is missing:

1. **Provenance.** We did not build these bytes. R-12's fallback ("use a
   prebuilt-binary vendor") is precisely the supply-chain risk SP-1 declined
   to take, and taking it here is a deliberate, time-boxed exception.
   (The release does publish `pdfium-attestation.json`; we do not verify it
   yet. **NOT IMPLEMENTED.**)
2. **Mobile.** Only `mac-arm64` is vendored. The two platforms that actually
   matter — `android-arm64` and `ios-arm64` — are not, and SP-1's *real* open
   question (revised criterion 2: does `ios_deployment_target = "16.0"` build
   and render?) is untouched. A prebuilt iOS 17 artefact cannot answer it.
3. **Static linking.** No static archive is published for macOS in this
   release, so the imported target is SHARED. SP-1's size criterion (F-5:
   stripped, GC'd contribution per architecture) is therefore unmeasured.
4. **A licence inventory generated from the tree we compiled.** We have the
   distributor's `licenses/` directory, which is good evidence and not the
   `gn desc`-derived inventory §7.8 requires.
5. **AGG.** `licenses/agg23.txt` says AGG **2.3**, permissive — consistent
   with §7.8's expectation and against the GPL 2.5 line. Carried forward as a
   residual risk for counsel, not closed here.
6. **Roll cadence.** SP-1 budgets ~0.5 ew/quarter to stay current with a
   branch-head dependency in our highest-risk parser. Nothing automates that
   yet.
