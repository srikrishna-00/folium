# SP-1 Findings — PDFium mobile build

**Status:** Investigated (desk research + upstream/recipe audit). No code built.
**Date:** 2026-09-29
**Spike definition:** `technical-requirements.md` §6, row SP-1. Box: 5 d.
**Blocks:** Phase 1 entry, Phase 5 entirely. Risk R-12. Estimate input for Phase 5 (78 ew).

---

## 1. The question as originally posed

> Can we build a trimmed PDFium (no XFA, no V8, no Skia) for `arm64-v8a` + `arm64-ios`
> **via CMake, without Chromium's GN/depot_tools**?
> **Pass:** static lib ≤ 12 MB, renders the fixture corpus, builds from our CMake.

The question bundles three separable things, and conflating them is what makes it look
binary:

| Sub-question | Answerable independently? |
|---|---|
| Q-A: Can PDFium be built for Android arm64 and iOS arm64 at all, trimmed as we want? | Yes |
| Q-B: Can it be built **without GN/depot_tools**, i.e. from a CMakeLists we own? | Yes |
| Q-C: Does the resulting binary fit the size and correctness budget? | Yes |

SP-1's pass criterion answers "no" if **any** of the three fails, which would have
condemned the whole PDF strategy on the strength of a build-system preference. The
findings below separate them.

## 2. Method

1. Read upstream's own build documentation (`pdfium.googlesource.com` README) for the
   mandated toolchain and the dependency solution it pulls.
2. Audited the only maintained-looking CMake port of PDFium (`madebr/pdfium-cmake`) for
   platform coverage, currency, and what it substitutes for vendored deps.
3. Audited the most actively maintained *mobile-producing* build recipe
   (`bblanchon/pdfium-binaries`) — specifically its configure step — to extract the exact
   GN arg set that is known to work in production, weekly, across Android and iOS.
4. Cross-checked a second independent prebuild pipeline (`Stirling-Tools/JPDFium`) for how
   static vs. component artefacts are shaped and what ends up inside a static archive.
5. Compared the recipes' platform floors against our proposed device floor (which
   `adr/0002` has since pushed *lower*).

## 3. Findings

### F-1 — PDFium has no standalone build, and no release tarball. (Q-B: hard)

Upstream's build is a `gclient` solution: `gclient config` → `gclient sync` → GN → Ninja,
with GN and Ninja supplied by depot_tools. There is no upstream CMake, no autotools, and
crucially **no release tarball** — versions are `chromium/NNNN` branch heads, not tags of a
distributable source archive. Anything "CMake-native" is therefore a third-party
re-expression of a build definition that upstream changes without notice.

### F-2 — The only serious CMake port is archived and never covered mobile. (Q-B: fails)

`madebr/pdfium-cmake` — the project that most closely matches the literal SP-1 ask — was
**archived (read-only) on 2024-05-01**. Its CI covered **Linux, macOS and Windows only**;
**Android and iOS were never tested targets**. Its own README states Skia and V8 support is
"implemented but not working", and it deliberately prefers **system packages over PDFium's
vendored libraries**.

Two consequences, and the second is the one that matters:

- Adopting it means taking ownership of an unmaintained port *and* forward-porting it to
  two mobile toolchains ourselves, then re-porting on every PDFium roll.
- Substituting system/Conan packages for the vendored tree **invalidates the §7.8 licence
  inventory** that SP-1 is supposed to produce. The whole point of that inventory is that
  it describes the exact code we ship. A CMake port that swaps FreeType/libjpeg/lcms for
  distro copies produces an inventory of a build we do not ship on mobile.

### F-3 — The real coupling is the dependency graph, not the build language.

PDFium's gclient solution pulls Chromium's `build/` tree plus **abseil, ICU, FreeType,
libjpeg-turbo, lcms, libopenjpeg and partition_alloc**. abseil is pinned to *the specific
copy Chromium uses*, via a GN rule that depends on Chromium's abseil target. Re-expressing
this in CMake means vendoring and version-tracking eight sub-projects whose versions are
chosen by Chromium, not by us.

**Mitigating finding:** `pdf_use_partition_alloc = false` is a supported and heavily
exercised build arg — the pdfium-binaries pipeline sets it unconditionally on every
platform including Android and iOS. That removes the single most Chromium-coupled
dependency from our build. It does not remove abseil or the `build/` tree.

### F-4 — Mobile builds are routine **with** GN. (Q-A: passes, with evidence)

`bblanchon/pdfium-binaries` has built PDFium automatically every Monday since 2017 and
currently ships **PDFium 156.0.8066.0 (`chromium/8066`)** for Android (arm, arm64, x64,
x86) and iOS (device arm64, simulator arm64, Mac Catalyst arm64/x64), among others. This
is a direct, continuously-verified existence proof for Q-A.

Its configure step uses effectively the exact argument set SP-1 wants:

```gn
is_debug                 = false
pdf_is_standalone        = true
pdf_use_partition_alloc  = false
pdf_enable_v8            = false      # T-105: no PDF JavaScript
pdf_enable_xfa           = false      # XFA requires V8 anyway
is_component_build       = false
treat_warnings_as_errors = false
target_cpu               = <arm64|arm|x64|x86>
target_os                = <android|ios>

# static-archive variant adds:
pdf_is_complete_lib      = true
use_custom_libcxx        = false

# android:
clang_use_chrome_plugins = false
default_min_sdk_version  = 23
use_mold                 = false

# ios:
ios_deployment_target    = "17.0"
ios_enable_code_signing  = false
use_blink                = true
clang_use_chrome_plugins = false
```

Note `pdf_use_skia` is **not set** — i.e. the default (AGG rasteriser) is what actually
ships, confirming `file-format-support.md` §7.8's "build with AGG unless SP-1 shows Skia is
needed". Nothing in the evidence suggests Skia is needed.

### F-5 — The 12 MB criterion is measuring the wrong artefact. (Q-C: needs restating)

Published Android/iOS artefacts are **≈3.1–3.4 MB gzipped shared libraries** per
architecture (e.g. `pdfium-ios-device-arm64.tgz` ≈ 3.15 MB, `pdfium-android-x86.tgz` ≈
3.15 MB on the 2026-09-07 release). Uncompressed `.so`/`.dylib` is materially larger; the
exact figure must be measured, not inferred.

More importantly, `pdf_is_complete_lib = true` produces **a single `libpdfium.a` containing
PDFium plus every bundled dependency** as unstripped, un-GC'd object code. That archive's
size is close to meaningless as a budget — the linker discards most of it. Measuring a
static archive against a 12 MB budget will either fail spuriously or pass meaninglessly.

**The criterion must be restated as: stripped, dead-code-eliminated *contribution to the
shipped binary*, measured with `-ffunction-sections -fdata-sections -Wl,--gc-sections` (and
`-dead_strip` on Apple), per architecture.** That is the number the 120 MB install budget
(`performance-strategy.md` §2.1) actually cares about.

### F-6 — The deployment-target floor is the live risk, and `adr/0002` just made it worse.

The maintained recipe targets **iOS 17.0** and **Android `minSdk` 23**. Our proposed floor
was iOS 16 / API 26, and **ADR 0002 decision 3 explicitly pushes the floor down further**
to include low-end/older devices.

- Android: `minSdk 23` is *below* our floor, so Android is not at risk here.
- **iOS is at risk.** iOS 17 is a config choice rather than an upstream hard requirement,
  but it means the only continuously-green recipe is not tested at iOS 16, and PDFium's
  iOS path uses `use_blink = true` — a less-trodden configuration than the Android one.
  An older iOS deployment target is, on the evidence, the **single most likely place this
  spike fails**, and the original spike definition does not test for it at all.

### F-7 — Licence inventory is *easier* under GN, not harder.

SP-1's blocking Phase 5 action item is a machine-generated inventory of every licence in
the pinned PDFium tree (`file-format-support.md` §7.8). Under GN + gclient, the pinned
tree is exactly described by `DEPS` at a commit, and `gn desc` gives the real link graph —
so the inventory is mechanical and, importantly, *describes what we ship*. Under a CMake
port that substitutes system packages, it does not. This inverts the intuition that owning
the CMakeLists gives us more control: for the licence question specifically, GN gives more.

Particular items still requiring manual confirmation at the pinned commit, unchanged from
§7.8: **FreeType — elect FTL, verify `docs/FTL.TXT`**; and **AGG 2.3 in `third_party/agg23`
— verify the vendored `LICENSE` is the permissive 2.4-and-earlier line, because AGG 2.5 is
GPL.** Neither is affected by the build-system decision.

## 4. Verdict against the original pass criterion

| Criterion | Result |
|---|---|
| Builds for `arm64-v8a` + `arm64-ios`, no XFA, no V8, no Skia | **PASS** (proven weekly upstream of us) |
| **Builds from our CMake, without GN/depot_tools** | **FAIL** — and pursuing it is a net loss |
| Static lib ≤ 12 MB | **UNDECIDED** — criterion measures the wrong artefact (F-5) |
| Renders the fixture corpus | **Untested** — requires the actual build; expected to pass |

So: the spike **fails as written and passes as intended.** The "no GN/depot_tools"
constraint was a proxy for a real goal — *we control our build and our supply chain* — and
that goal is better served a different way.

## 5. Recommendation

**Reframe SP-1. Do not write a CMake port of PDFium.**

**Build PDFium from pinned source, with upstream's own GN/depot_tools toolchain, inside a
hermetic container, as an out-of-tree artefact build in our own CI. Consume the result from
our CMake as an `IMPORTED` static target.**

This is explicitly **not** "use a prebuilt-binary vendor" (the R-12 fallback that adds
supply-chain risk). We build it ourselves, from source we pin by commit hash, with our own
args, and we generate the licence inventory from the tree we actually compiled. CMake
remains the build system of record for everything we write; PDFium is a dependency with a
different, contained build.

### Revised pass criteria for SP-1

1. A pinned, containerised, reproducible PDFium build produces static archives for
   `android-arm64`, `android-arm` and `ios-arm64` (+ simulator), from the F-4 arg set.
   *Confidence: high.*
2. **`ios_deployment_target = "16.0"` (or whatever `adr/0002`'s low-end decision settles
   on) builds and renders correctly.** *This is the real test. Confidence: medium.*
3. Stripped, GC'd contribution to the linked binary ≤ 12 MB per architecture (F-5).
4. The fixture corpus renders, compared against reference images.
5. `gn desc`-derived licence inventory is machine-generated and checked in (§7.8).
6. A documented, scripted **re-roll procedure** — because upstream has no stable releases.

### Estimate impact

- **Box: 5 d → 8 d.** The added 3 d are the container/CI hermetic build and the iOS-16
  validation, neither of which the original definition covered.
- **Phase 5 (78 ew): unchanged**, but assumption 3 in `engineering-complexity-estimate.md`
  ("a failed SP-1 adds 15–30 ew") now has a much lower probability of firing.
- **New standing cost, not previously budgeted:** PDFium roll maintenance. Upstream ships
  branch heads, not releases; CVE response in a parser we called our highest technical risk
  requires staying current. **Budget ≈ 0.5 ew per quarter, ongoing, from Phase 5.**

### Risk register impact

- **R-12** ("PDFium cannot be built for mobile outside Chromium's GN/depot_tools within
  budget") — the premise is now answered: *correct, it cannot, and it should not be tried.*
  Rewrite R-12 as **"PDFium cannot be built for our iOS deployment-target floor"**, score
  M×4 → **L×4 = 4**, owner Core lead, gated by revised criterion 2. The MuPDF-commercial-
  licence fallback stays available but is no longer the likely path.

## Sources

- [PDFium — Building PDFium (upstream README)](https://pdfium.googlesource.com/pdfium/+/master/README.md)
- [madebr/pdfium-cmake (archived 2024-05-01)](https://github.com/madebr/pdfium-cmake)
- [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
- [bblanchon/pdfium-binaries — `steps/05-configure.sh`](https://raw.githubusercontent.com/bblanchon/pdfium-binaries/master/steps/05-configure.sh)
- [bblanchon/pdfium-binaries — releases](https://github.com/bblanchon/pdfium-binaries/releases)
- [Stirling-Tools/JPDFium — prebuild release notes (static vs. component tarballs)](https://github.com/Stirling-Tools/JPDFium/releases/tag/pdfium-20260921-f5ca813)
- [Using abseil in PDFium (pdfium mailing list)](https://groups.google.com/g/pdfium/c/StGeDwchmo0)
- [`build_overrides/partition_alloc.gni` — pdfium](https://pdfium.googlesource.com/pdfium/+/refs/heads/main/build_overrides/partition_alloc.gni)
