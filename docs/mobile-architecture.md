# Mobile Architecture — Android & iOS

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §25. Android is Kotlin + JNI/NDK over the C/C++ core; iOS is Swift with an
Objective-C/C bridge over the same core.

---

## 1. Shared shape

Both platforms follow the same three-tier structure, so that a feature is implemented once in
the core and twice in thin, idiomatic shells:

```
┌───────────────────────────────────────────────┐
│ UI          Compose (Android) / SwiftUI (iOS) │  ← platform-specific, not shared
├───────────────────────────────────────────────┤
│ App layer   ViewModels, navigation, DI,       │  ← platform-specific, thin
│             platform services (camera, files) │
├───────────────────────────────────────────────┤
│ Binding     uapp-kotlin (JNI) / UAPPKit (ObjC)│  ← hand-written, ~2-4k lines each
├───────────────────────────────────────────────┤
│ libuapp_capi  (C99)                           │  ← the single supported boundary
├───────────────────────────────────────────────┤
│ C++17 core (shared, identical on both)        │
└───────────────────────────────────────────────┘
```

**Where the line sits, and why.** Everything with a correctness requirement lives in the core:
document model, sync, conflict resolution, parsing, indexing, crypto, policy. Everything with a
platform-feel requirement lives above: navigation, gestures, animation, accessibility, camera,
system integration. Business logic never leaks upward; UI concerns never leak downward. The
test of this is that a new platform should need no core changes.

**Bindings are hand-written, not generated.** Generated bindings (SWIG, djinni) produce
mechanical APIs that are unpleasant in both Kotlin and Swift, hide lifetime semantics, and are
hard to debug at the boundary — which is exactly where bugs will be. Two hand-written shims of
a few thousand lines each is the cheaper total cost.

## 2. The C ABI contract (recap)

Defined in `api-design.md` §10. The rules that matter for the bindings:

1. Only C99 types and opaque pointers cross.
2. Structs carry a leading `uint32_t struct_size` for forward compatibility.
3. Every allocation crossing the boundary has a matching `uapp_*_free`.
4. Complex payloads (block trees, search results) are FlatBuffers in a `uapp_buf_t`.
5. Nothing blocks indefinitely; long work is a job with progress and cancellation.
6. Errors are `uapp_error_t` with a stable code; the binding maps to
   `Result`/`throws`.

## 3. Android

### 3.1 Stack

| Layer | Choice |
|---|---|
| Language | Kotlin 2.x (Java only where a library forces it) |
| UI | Jetpack Compose + Material 3 |
| Architecture | MVVM + unidirectional data flow; `ViewModel` + `StateFlow` |
| DI | Hilt |
| Async | Coroutines + Flow; core jobs bridged to `suspend` functions |
| Camera | CameraX |
| Background | WorkManager; foreground service for user-initiated long jobs |
| Files | Storage Access Framework; scoped storage; `FileProvider` for outbound sharing |
| Secure storage | Android Keystore (StrongBox where available) + `EncryptedSharedPreferences` |
| Biometrics | `androidx.biometric` |
| Build | Gradle (AGP 8.x) + externalNativeBuild → CMake |
| Min / target | API 26 / latest (min pending PQ-7) |
| ABIs | `arm64-v8a` primary; `x86_64` for emulator/CI. **`armeabi-v7a` deferred** — see §3.7. |

### 3.2 JNI boundary rules

JNI is where Android apps with native cores crash. The rules are fixed now.

**Lifetime.** The Kotlin side owns a `long` handle. Every native object is wrapped in a class
implementing `AutoCloseable`, and additionally registered with a `Cleaner` as a backstop:

```kotlin
class PdfDocument private constructor(private var handle: Long) : AutoCloseable {
    private val cleanable = CLEANER.register(this, CloseAction(handle))
    override fun close() { cleanable.clean() }   // idempotent
    // every method checks handle != 0 and throws IllegalStateException otherwise
}
```
`Cleaner` is a safety net for leaks, **not** the primary mechanism — finalisation timing is
unpredictable and a 200 MB PDF must be released deterministically.

**Threading.**
- No JNI call happens on the main thread. Enforced by making every binding function `suspend`
  on `Dispatchers.Default`/`IO`, plus a debug-build assertion that throws on main-thread entry.
- Native → Java callbacks use a cached `JavaVM*` and `AttachCurrentThread` / `DetachCurrentThread`.
  Worker threads that call back are attached as **daemon** threads so they cannot block JVM exit.
- `jclass`/`jmethodID` are cached in `JNI_OnLoad` with global references. Looking them up per
  call is a measurable cost in a hot path.

**Data transfer.**
- **Bitmaps:** allocate the `android.graphics.Bitmap` in Kotlin, lock its pixels with
  `AndroidBitmap_lockPixels`, and have the core render **directly into that buffer**. No copy.
  This is the single most important performance decision on the Android side — copying a 4 MB
  page bitmap per frame is the difference between smooth and janky scrolling.
- **Large byte payloads:** `ByteBuffer.allocateDirect` + `GetDirectBufferAddress`.
- **Strings:** `GetStringUTFChars` is UTF-8-with-surrogate-quirks (modified UTF-8). We pass
  strings as UTF-8 byte arrays, not `jstring`, to avoid the encoding trap entirely.
- **Structured data:** FlatBuffers in a direct `ByteBuffer`, read in place.

**Errors.** The core never throws. The binding checks the status, reads
`uapp_last_error()` and throws a typed `UappException(code, domain, message)`. Kotlin code
switches on the code enum, never on the message.

**Exception hygiene.** Every JNI function checks `ExceptionCheck()` after any JNI call that can
throw, and returns immediately. A pending exception left unchecked corrupts the VM in
non-obvious ways.

### 3.3 Camera and scanner pipeline

```
CameraX Preview + ImageAnalysis (YUV_420_888, backpressure: KEEP_ONLY_LATEST)
   │  analysis frames at ~10 fps, downscaled to ~640 px long edge
   ▼
JNI: pass the Y plane by direct buffer (no copy, no RGB conversion)
   ▼
core scanner/edge-detection → quad in normalised coords
   ▼
back to Compose: draw the quad overlay
   │
   │  on capture:
   ▼
ImageCapture → full-resolution JPEG → core: perspective + enhancement + (optional) OCR
```

Only the **luma plane** is sent for detection — edge detection does not need colour, and
skipping YUV→RGB conversion on every frame saves both CPU and allocation. Capture uses the
full-resolution path.

### 3.4 Storage

- Content DB and blobs in app-private internal storage (`filesDir`). Never external storage.
- `android:allowBackup="false"` and no `dataExtractionRules` for the DB (T-203).
- SAF for import/export; we never ask for broad storage permissions.
- Blob cache honours `StorageManager` allocation APIs so the OS can reclaim it under pressure.

### 3.5 Background

- `WorkManager` with `Constraints` (unmetered for large transfers, battery-not-low for indexing)
  for sync and indexing.
- A **foreground service** with a progress notification for user-initiated long jobs (batch OCR,
  large conversion) — the only reliable way to get sustained CPU under Doze.
- Every job checkpoints (`offline-architecture.md` §7).

### 3.6 Packaging

- Android App Bundle with ABI splits; `arm64-v8a` users do not download other ABIs.
- Native debug symbols uploaded separately (`debugSymbolLevel = FULL`), stripped from the
  shipped `.so`.
- **16 KB page-size alignment is mandatory** for newer Android devices — verified in CI (NFR-9).
- R8 full mode; `-keep` rules for JNI-referenced classes and methods, with a CI check that JNI
  symbol lookups still resolve in the release build. A minification-broken JNI lookup is a
  release-only crash, which is the worst kind.
- Target size: base APK ≤ 40 MB, install ≤ 120 MB before optional models.

### 3.7 32-bit deferral

`armeabi-v7a` is deferred, not dropped. Reasons: a 32-bit address space makes the
memory-mapping strategy for large PDFs (`mmap_size = 256 MB`) untenable, and mid-tier 32-bit
devices cannot run local AI anyway. If market data (PQ-7) shows a material 32-bit tail, we add
it with reduced limits and local AI disabled, and we say so in the store listing.

## 4. iOS

### 4.1 Stack

| Layer | Choice |
|---|---|
| Language | Swift 5.9+; Objective-C++ (`.mm`) only in the bridge |
| UI | SwiftUI for chrome; **UIKit for the document/PDF canvas** |
| Architecture | MVVM with `@Observable`; Swift Concurrency (`async/await`, actors) |
| Camera | AVFoundation (`AVCaptureVideoDataOutput` + `AVCapturePhotoOutput`) |
| Background | `BGProcessingTask`, `BGAppRefreshTask`, `URLSession` background config |
| Files | `UIDocumentPickerViewController`, security-scoped URLs, Files app integration |
| Secure storage | Keychain (`kSecAttrAccessibleWhenUnlockedThisDeviceOnly`) + Secure Enclave |
| Biometrics | LocalAuthentication |
| Build | Xcode + SwiftPM; the core is an XCFramework produced by CMake |
| Min | iOS 16 (pending PQ-7) |
| Arch | `arm64` device, `arm64` simulator; `arm64` macOS slice for Catalyst/desktop later |

### 4.2 Why UIKit for the canvas

SwiftUI's `ScrollView` does not give the control needed for a 500-page tiled document view:
no reliable tile lifecycle, no `CATiledLayer` equivalent, limited zoom control, and no way to
cancel off-screen render work precisely. The canvas is a `UIScrollView` + `CATiledLayer`
subclass wrapped in `UIViewRepresentable`. Everything else is SwiftUI. This is a deliberate,
scoped exception, not a general retreat from SwiftUI.

### 4.3 Bridge design

Three layers, because Swift cannot safely consume the C API directly at scale:

```
Swift app code
   │   idiomatic, async, typed, ARC-managed
   ▼
UAPPKit (Swift)         — classes wrapping handles, async/await, typed errors
   │
   ▼
UAPPCore (ObjC++ .mm)   — thin translation: C structs ⇄ ObjC objects, callbacks → blocks
   │
   ▼
libuapp_capi (C)
```

Why the ObjC++ middle layer rather than Swift-calls-C directly:
- Swift's C interop makes every pointer `UnsafeMutablePointer<T>?`, which pollutes call sites
  and makes lifetime errors easy.
- Callbacks with `void*` user data are awkward to bridge to Swift closures safely; ObjC blocks
  map cleanly.
- `.mm` lets us `#include` the C++ internals directly for a few performance-critical paths
  without exposing C++ to Swift at all.

**Lifetime.** Each Swift wrapper holds the handle and frees it in `deinit`. ARC gives
deterministic release, so unlike Android there is no need for a `Cleaner` equivalent. Handles
are non-copyable in spirit: wrapper classes are `final` and not `Sendable` unless the
underlying handle is documented thread-safe.

**Concurrency.** Core jobs are wrapped in `withCheckedThrowingContinuation`; cancellation maps
Swift's `Task.cancel()` to `uapp_job_cancel()`. Callbacks arrive on core worker threads and are
hopped to `@MainActor` by the wrapper — never by the caller.

**Errors.** `uapp_error_t` → a Swift `enum UAPPError: Error` with the code as an associated
value, so `catch UAPPError.pdf(.encryptedPasswordRequired)` is expressible.

**Rendering.** The core renders into a `CGContext`-compatible buffer allocated Swift-side
(`CGBitmapContextCreate` backing), so there is no copy on the page-render path — the iOS
analogue of the Android Bitmap decision.

### 4.4 Building the core for iOS

CMake with `CMAKE_SYSTEM_NAME=iOS`, producing static libraries per slice, assembled into an
**XCFramework** (device arm64 + simulator arm64). XCFramework rather than a fat framework
because `lipo`-ing device and simulator arm64 together is no longer possible.

Distributed to the app target via a local SwiftPM `binaryTarget`. Bitcode is not produced
(deprecated by Apple).

### 4.5 Platform OCR and vision

Apple Vision (`VNRecognizeTextRequest`) is registered as an `OCRProviderPort` implementation
and is the **default on iOS**: better accuracy on printed text, faster, no language-pack
download, no binary-size cost. Tesseract remains compiled in as the guaranteed floor and for
languages Vision does not cover. The user sees one "OCR" feature; the provider is an
implementation detail surfaced only in diagnostics.

### 4.6 Background execution

`BGProcessingTask` for sync/indexing (requires `requiresNetworkConnectivity` /
`requiresExternalPower` flags set honestly), `BGAppRefreshTask` for light refresh,
`beginBackgroundTask(withName:)` for a short grace window when the user backgrounds mid-job.

**We assume none of these will run promptly.** Everything checkpoints
(`offline-architecture.md` §7). A design that depends on iOS granting background time is a
design that loses work.

### 4.7 App Store considerations

- **Licensing:** no GPL/AGPL/LGPLv3 code in the binary (`file-format-support.md` §7.2). This
  constraint drives several dependency choices and is not negotiable.
- **Privacy manifest** (`PrivacyInfo.xcprivacy`) declaring data types collected and
  required-reason APIs (file timestamps, disk space, user defaults). Third-party SDKs must ship
  their own.
- **Encryption export compliance:** we use non-exempt encryption; `ITSAppUsesNonExemptEncryption`
  declared, with a CCATS/exemption determination on file.
- **App size:** cellular download limit means the base app must stay well under it; models are
  on-demand (`offline-architecture.md` §5). On-Demand Resources is an option for language packs.
- **Purpose strings** for camera, photo library, Face ID, and notifications, written to actually
  explain the use — vague strings are a common rejection cause.

## 5. Shared UI/UX architecture

Not a shared codebase — a shared *specification*, so the two apps stay consistent without a
cross-platform framework:

- **Design tokens** (colour, type scale, spacing, radii, motion) defined once in a JSON source
  of truth and generated into a Compose theme and a SwiftUI theme. This is where consistency
  actually comes from.
- **Navigation model** specified as a shared state machine; implemented natively.
- **Component inventory** with identical names and behaviour contracts on both platforms.
- **Screen specs** define behaviour and states (including every offline state from
  `offline-architecture.md` §10); each platform implements them natively.

Both apps must feel native. An app that feels like a port on one platform loses on that
platform.

## 6. Platform service injection

The core never reaches for platform APIs. The host injects a vtable at init:

```c
typedef struct {
    uint32_t struct_size;
    /* secure storage */
    uapp_status_t (*keystore_get)(void* ctx, const char* key, uapp_buf_t* out);
    uapp_status_t (*keystore_put)(void* ctx, const char* key, const uint8_t*, size_t);
    uapp_status_t (*keystore_delete)(void* ctx, const char* key);
    /* network */
    uapp_status_t (*http_request)(void* ctx, const uapp_http_req_t*, uapp_http_cb, void*);
    bool          (*is_online)(void* ctx);
    uint8_t       (*network_class)(void* ctx);   /* offline|metered|unmetered */
    /* filesystem */
    uapp_status_t (*open_scoped)(void* ctx, const char* url, uapp_stream_t** out);
    /* platform OCR */
    uapp_status_t (*platform_ocr)(void* ctx, const uapp_image_t*, uapp_ocr_result_t*);
    /* misc */
    void          (*log)(void* ctx, int level, const char* domain, const char* msg);
    uint64_t      (*monotonic_ms)(void* ctx);
    void          (*random_bytes)(void* ctx, uint8_t*, size_t);
    void*         ctx;
} uapp_platform_vtable_t;
```

This is the seam that makes the core testable with fakes and keeps `jni.h` and
`Foundation/*` out of the core entirely (`architecture.md` §8 rule 4).

## 7. Performance notes specific to mobile

| Concern | Approach |
|---|---|
| Page rendering | Tiled `CATiledLayer` (iOS) / custom tile view (Android); render at the current zoom only; cancel off-screen tiles immediately |
| Bitmap memory | Pooled buffers reused across pages; hard cap on the decoded-page cache (LRU, ~6 pages) |
| Scroll smoothness | Never allocate on the render path after warm-up (NFR-5); prefetch ±2 pages on a background thread |
| List performance | Windowed lists over paged SQL queries; never load a whole block tree for a long document |
| Startup | Lazy-init everything not needed for the first screen; the core's heavy subsystems (OCR, AI) initialise on first use |
| Battery | Coalesce sync; no polling timers; respect low-power mode by deferring indexing |
| Thermals | Long jobs monitor thermal state (`ProcessInfo.thermalState` / `PowerManager.getCurrentThermalStatus`) and throttle themselves rather than being throttled |

## 8. Testing on mobile

| Level | Android | iOS |
|---|---|---|
| Core unit | GoogleTest, run on-device via an instrumented harness | GoogleTest in an XCTest host |
| Binding | Instrumented tests exercising every JNI entry point, including error and cancellation paths | XCTest over UAPPKit |
| Leak/lifetime | Run the binding suite under ASan; assert zero leaked handles at teardown | Instruments Leaks + ASan |
| UI | Compose UI tests | XCUITest |
| E2E journeys | Both platforms, including full airplane-mode runs |
| Device matrix | 1 low-end, 1 mid, 1 flagship + 1 tablet per platform, on real hardware |

**JNI and bridge code get their own leak tests.** A handle leak at the boundary is invisible to
both the JVM profiler and Instruments unless it is specifically looked for.

## 9. Open mobile questions

| # | Question |
|---|---|
| MQ-1 | Minimum Android API and iOS version (= PQ-7). Drives the local-AI floor. |
| MQ-2 | Ship `armeabi-v7a`? Depends on target-market device data. |
| MQ-3 | Is a tablet-optimised layout v1.0 or post-1.0? It is real, separable work. |
| MQ-4 | Do we use ML Kit on Android (better OCR, but requires Play Services and excludes devices without it) or Tesseract everywhere for consistency? |
| MQ-5 | Apple Pencil / stylus pressure support for annotation — v1.0 or later? |
