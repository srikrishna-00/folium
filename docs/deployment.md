# Build, Release & Deployment

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Build and release pipeline for a monorepo containing a C/C++ core, two mobile apps, a desktop
app, a web client, and a cloud backend.

---

## 1. The shape of the problem

Six artefact families with different toolchains, different release cadences, and one
hard constraint: **the core must be byte-identical across every consumer of a given version.**

| Artefact | Toolchain | Cadence | Rollback |
|---|---|---|---|
| Core static libs | CMake + Clang | per commit | n/a (consumed) |
| Android app (AAB) | Gradle + NDK | weekly → biweekly | staged rollout halt; no true rollback |
| iOS app (IPA) | Xcode + SwiftPM | weekly → biweekly | phased release pause; **no rollback at all** |
| Desktop | Gradle (Compose MP) | monthly | auto-update to a previous build |
| Web | Vite/TS + WASM | daily | instant (CDN pointer) |
| Backend | Go + Docker | daily | instant (redeploy previous image) |

**The asymmetry is the whole design constraint.** The backend can be rolled back in 60 seconds;
a bad iOS release cannot be rolled back at all and takes a day or more to replace. Therefore:

1. The server must support the **previous two** client versions at all times.
2. Client-breaking changes ship server-first, behind a flag, and are enabled only after client
   adoption is measured.
3. Anything risky on a client ships behind a **remote kill switch**, because the kill switch is
   the only rollback mechanism mobile has.

## 2. Repository layout (build-relevant)

```
/                      CMakeLists.txt (root), CMakePresets.json
  third_party/         vendored deps, pinned by commit + SHA256, each with LICENSE
  cmake/               toolchain files: android.cmake, ios.cmake, wasm.cmake, host.cmake
  core/ file-engine/ pdf-engine/ scanner/ ocr/ media/ ai/ knowledge/
  storage/ network/ security/ api/
  mobile/android/      Gradle project; externalNativeBuild → CMake
  mobile/ios/          Xcode project; consumes UAPPCore.xcframework
  desktop/  web/
  server/              Go modules (separate from the CMake build)
  tests/ benchmarks/ docs/
  tools/               build, release, notice-generation, ABI-diff scripts
  .github/workflows/   CI
```

## 3. Core build

### 3.1 CMake presets

```
host-debug          Linux/macOS, ASan+UBSan, tests on
host-release        optimised, LTO
android-arm64       NDK r26+, API 26, arm64-v8a
android-x86_64      emulator/CI
ios-device          arm64, iOS 16
ios-simulator       arm64 simulator
wasm                Emscripten, reduced feature set
```

Every preset builds from the same source with no `#ifdef` forests. Platform differences live in
adapters, not in conditional compilation inside engines.

### 3.2 Flags

```
Common:   -std=c++17 -fno-exceptions -fno-rtti
          -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
          -fstack-protector-strong -D_FORTIFY_SOURCE=2
          -fvisibility=hidden -fvisibility-inlines-hidden
Release:  -O2 -flto=thin -ffunction-sections -fdata-sections
          (link) --gc-sections, -dead_strip on Apple
Debug:    -O0 -g3 -fsanitize=address,undefined
Repro:    -ffile-prefix-map=$(pwd)=. ; SOURCE_DATE_EPOCH from the commit date
```

- `-fno-exceptions -fno-rtti`: decided in `adr/0001`. Errors are return codes; the C ABI cannot
  carry exceptions anyway, and it removes a large chunk of binary size.
- `-fvisibility=hidden` with explicit `UAPP_EXPORT` on the C ABI keeps the exported symbol
  surface exactly equal to the documented ABI — which is what makes the ABI-diff check meaningful.

### 3.3 Third-party policy

- Vendored under `third_party/<name>/`, pinned by commit hash **and** SHA-256, both verified at
  configure time.
- No network access during the build. A build that cannot run offline is not reproducible.
- Each directory carries `LICENSE` and a `METADATA.yaml` (name, version, SPDX id, upstream URL,
  CVE-tracking owner, patches applied).
- Patches live in `third_party/<name>/patches/` and are applied by the build, never committed
  as modified vendor source — so upgrading upstream is mechanical.

### 3.4 Determinism

Same source + same toolchain → byte-identical output, verified by a CI job that builds twice on
different machines and compares hashes. Without this, "which build is in production?" becomes
unanswerable, and supply-chain attestation is theatre.

## 4. Mobile builds

### Android
```
Gradle → externalNativeBuild → CMake (arm64-v8a, x86_64)
  → strip .so, upload native symbols separately
  → R8 full mode (with verified JNI keep rules)
  → AAB with ABI splits
  → sign with Play App Signing
  → 16 KB page-alignment check (NFR-9)
```

### iOS
```
CMake → static libs per slice → XCFramework (device arm64 + simulator arm64)
  → SwiftPM binaryTarget → Xcode archive
  → dSYM uploaded to the crash service
  → sign + notarise (macOS) / App Store Connect upload (iOS)
```

The XCFramework is built once per core commit and cached; app-only changes do not rebuild the
core. This matters: a cold core build is 15–25 minutes and would otherwise dominate every iOS PR.

## 5. Versioning

**Product version:** SemVer `MAJOR.MINOR.PATCH`, shared by all clients of a release train.
**Core ABI version:** independent `MAJOR.MINOR` (`api-design.md` §10).
**REST API version:** independent path major (`api-design.md` §2).
**Sync protocol version:** independent integer (`sync-architecture.md` §9).

Four independent version axes is not over-engineering — they change for different reasons, and
coupling them forces unnecessary major bumps. The compatibility matrix is published and tested:

| Client | Server REST | Sync protocol |
|---|---|---|
| 1.x | v1 | 1 |
| 2.x | v1, v2 | 1, 2 |

## 6. Branching and release trains

- `main` is always releasable. Trunk-based; short-lived feature branches; feature flags for
  anything incomplete.
- `release/x.y` cut from `main` at the start of a release train; fixes cherry-picked *to* it,
  never merged back as a divergent line.
- Tags: `v1.2.3` (product), `core-abi-2.1`.
- **Hotfix path:** branch from the release tag, fix, ship. Documented and rehearsed before v1.0,
  because the first time you need it should not be the first time you do it.

## 7. Release pipeline

```
 tag vX.Y.Z on release/x.y
      │
      ├─ full CI: all tests, all sanitizers, benchmarks, 72 h fuzz result check
      ├─ licence check + THIRD_PARTY_NOTICES regenerated from the link graph
      ├─ SBOM (CycloneDX) generated from the actual dependency graph
      ├─ ABI diff vs. previous release
      ├─ reproducibility check (two independent builds)
      │
      ├─ build: Android AAB · iOS IPA · desktop · web bundle · server image
      ├─ sign everything (release keys in an HSM)
      ├─ upload symbols (native .so symbols, dSYM, WASM source maps)
      │
      ├─ Backend: deploy to staging → smoke → canary 5 % → 25 % → 100 %
      ├─ Web: deploy to a preview URL → smoke → flip the CDN pointer
      ├─ Android: internal → closed → open beta → staged production 1/5/20/50/100 %
      ├─ iOS: TestFlight internal → external → phased release (7 days)
      └─ Desktop: beta channel → stable
```

**Servers first, always.** The server must speak the new protocol before any client that uses
it exists in the wild.

## 8. Staged rollout and halt criteria

Automatic halt (and, for backend, automatic rollback) on any of:

| Signal | Threshold |
|---|---|
| Crash-free sessions | < 99.3 % (baseline 99.5 %) |
| ANR rate (Android) | > 0.4 % |
| API 5xx rate | > 0.5 % |
| Sync failure rate | > 2 % |
| **Any data-loss report** | **1** |
| p95 cold start | > 1.25 × baseline |
| Store rating in the rollout cohort | drops > 0.3 |

Halting a staged rollout is cheap and expected. It is not treated as a failure.

## 9. Feature flags and kill switches

- Remote config, fetched at start and refreshed periodically, cached locally so it works offline.
- **Every risky subsystem has a kill switch:** local AI, cloud AI per provider, OCR provider
  selection, semantic search, each conversion path, agent framework, sync protocol version.
- Flags default to **safe-off** when config cannot be fetched. A device that has never reached
  the server gets the conservative path.
- Flags are not permanent architecture. Each carries an owner and a removal date; stale flags
  are cleaned up every release train.

## 10. Backend deployment

- Containerised Go services; immutable images tagged by commit SHA.
- Kubernetes (or equivalent) with rolling deploys, readiness/liveness probes, resource limits.
- **Conversion workers are one-shot, network-denied, and resource-capped** — the server-side
  substitute for the process isolation we cannot have on mobile (`architecture.md` §5).
- DB migrations are **expand/contract**: add the new shape, dual-write, backfill, switch reads,
  then remove the old shape in a later release. A migration is never deployed in the same
  release as the code that requires it.
- Secrets from a managed secret store, never in images or env files committed anywhere.
- Backups: continuous WAL archiving + daily snapshots; **restore is rehearsed quarterly**, since
  an untested backup is not a backup.

## 11. Store submission

| | Android | iOS |
|---|---|---|
| Review time | hours–days | 1–3 days typical |
| Data declarations | Play Data Safety | Privacy nutrition labels + `PrivacyInfo.xcprivacy` |
| Permissions justification | camera, notifications, foreground service type | camera, photos, Face ID purpose strings |
| Encryption | — | `ITSAppUsesNonExemptEncryption` + export compliance on file |
| Licence compliance | in-app OSS licence screen | same; **no GPL/AGPL/LGPLv3 in the binary** |
| Size | AAB with ABI splits | on-demand models to stay under the cellular limit |

Submission metadata (release notes, screenshots, store listings) is version-controlled and
reviewed like code.

## 12. Observability of releases

Every release emits a build manifest (commit, dependency versions, flags, ABI version) that
clients report in diagnostics. Given a crash report, it must be possible to identify the exact
source tree and dependency set in one lookup. Symbol files for every shipped build are retained
for at least 24 months — longer than the client support window, because old versions crash too.

## 13. Local developer experience

```
./tools/bootstrap.sh          # toolchains, hooks, LFS, fixture fetch
cmake --preset host-debug && cmake --build --preset host-debug
ctest --preset host-debug
./tools/build-android.sh      # or open mobile/android in Android Studio
./tools/build-ios.sh          # builds the XCFramework, then open the Xcode project
```

Targets: `bootstrap` ≤ 10 minutes on a clean machine; incremental core build ≤ 30 s;
core unit suite ≤ 2 minutes. If these degrade, developers stop running tests locally, and the
CI pipeline becomes the only feedback — which is too slow to be useful.

## 14. Open deployment questions

| # | Question |
|---|---|
| DQ-1 | Cloud provider and region strategy — data-residency requirements materially change the topology. |
| DQ-2 | Which crash-reporting service? It must be configurable to never receive document content. |
| DQ-3 | Desktop distribution: own auto-updater, or Mac App Store + Microsoft Store (which brings sandboxing constraints)? |
| DQ-4 | Monorepo CI cost: full matrix per PR is expensive. Which jobs are PR-blocking vs. nightly? (§13 of `testing-strategy.md` proposes a split.) |
| DQ-5 | Do we self-host model weights on our CDN, or fetch from a third party? Affects offline reliability and licence obligations. |
