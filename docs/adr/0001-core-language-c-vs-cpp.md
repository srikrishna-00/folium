# ADR 0001 — Core language: C vs. C++

- **Status:** Proposed — awaiting sign-off before Phase 1
- **Date:** 2026-09-29
- **Deciders:** Lead architect (proposed); requires approval from the engineering owner
- **Supersedes:** —
- **Related:** `docs/architecture.md`, `docs/technical-requirements.md`, `docs/mobile-architecture.md`

Spec §40 item 5 names this as a distinct decision point. This ADR settles it.

---

## Context

Spec §2 mandates a "C/C++-first architecture" but explicitly declines to choose between them:
*"Do NOT force every component to be written in C."* The choice is load-bearing because it
determines:

- how we express ownership and lifetimes across a large native codebase;
- what crosses the boundary to Kotlin and Swift;
- what our third-party dependencies force on us;
- how fuzzing, sanitizers and static analysis behave;
- what a new engineer must know to be productive.

### Forces

1. **The exported boundary must be C regardless.** JNI speaks C. Swift's C interop is
   first-class; its C++ interop is newer, compiler-version-sensitive, and not something to bet a
   two-platform product on. The C++ ABI is not stable across toolchains. **So a C ABI is not a
   choice — it is a constraint.** The real question is what lies behind it.
2. **Our dependency set is mixed.** SQLite, libjpeg-turbo, libpng, libtiff, zlib, expat,
   libsodium and Leptonica are C. PDFium, QPDF, Tesseract, OpenCV, xlnt, HarfBuzz, llama.cpp
   and ONNX Runtime are C++. A pure-C core would need C shims around every C++ dependency —
   and PDFium and Tesseract are two of the three most important dependencies in the product.
3. **The domain is heavily resource-owning.** File handles, memory arenas, database
   transactions, bitmaps, model contexts, locks. Manual paired cleanup across dozens of early
   returns in every parser is the single most reliable source of leaks and double-frees in C
   codebases.
4. **Untrusted input is the primary threat** (spec §29, `security-model.md` TB-1). Whatever
   reduces memory-safety defects in our own code is worth a great deal.
5. **The core is large** — roughly 150–250 kLOC projected across 12 modules. This is above the
   size where C's lack of abstraction stops being simplicity and starts being repetition.
6. **Binary size matters** (NFR-4: ≤ 25 MB for the core). C++ exceptions, RTTI and
   template-heavy standard-library use all cost bytes.

## Decision

**Implement the core in C++17, restricted to a disciplined subset, and export a pure C99 ABI.**

```
┌──────────────────────────────────────────────┐
│  Kotlin / Swift / TypeScript                 │
├──────────────────────────────────────────────┤
│  libuapp_capi — C99. Opaque handles.         │  ← the only supported boundary
│  No exceptions, no STL, no C++ types cross.  │
├──────────────────────────────────────────────┤
│  C++17 internals (restricted subset)         │
├──────────────────────────────────────────────┤
│  Mixed C and C++ third-party dependencies    │
└──────────────────────────────────────────────┘
```

### The subset

**Used:**
- RAII and deterministic destruction — the primary reason for this decision
- `std::unique_ptr`; `std::shared_ptr` only where sharing is genuinely required and documented
- `std::string_view`, `std::span`, `std::optional`, `std::variant`
- Templates for containers and algorithms; `constexpr`
- Strong typedefs and enum classes for IDs and error codes
- Namespaces; `final` classes; free functions in preference to deep hierarchies

**Forbidden:**
- **Exceptions** (`-fno-exceptions`). Errors are returned as `uapp_status_t`, all fallible
  returns `[[nodiscard]]`. Exceptions cannot cross the C ABI, and half the codebase would need
  `noexcept` boundaries anyway. Allocation failure is handled explicitly.
- **RTTI** (`-fno-rtti`). No `dynamic_cast`, no `typeid`.
- **Inheritance for implementation reuse.** Interfaces are abstract classes with pure virtuals
  (ports) or function-pointer vtables (the platform boundary). No deep hierarchies.
- **Operator overloading** beyond comparison and arithmetic on value types.
- **`std::iostream`** — heavy, slow, and it pulls in locale machinery.
- **Template metaprogramming beyond what a reader can follow in one sitting.**
- **Implicit conversions** (`-Wconversion` is on and `-Werror`).
- **`new`/`delete` in application code.** Factory functions returning `unique_ptr`, or arena
  allocation for parse jobs.

### Consequence for the C boundary

Every exported function is `extern "C"`, takes and returns only C99 types and opaque pointers,
and wraps its body in a translation layer that converts internal failures (including a
`std::bad_alloc` equivalent, since exceptions are off — allocation failure returns null and is
checked) into status codes. Nothing escapes.

## Alternatives considered

### A — Pure C99/C11 throughout

**For:**
- One language, unambiguous ABI, trivially callable from everywhere.
- Simplest possible mental model; smallest binaries; fastest compiles.
- Matches the most literal reading of "C/C++-first".
- No hidden control flow, no hidden allocation, no template bloat.

**Against:**
- **PDFium, Tesseract, OpenCV, xlnt, HarfBuzz, llama.cpp and ONNX Runtime are C++.** We would
  write and maintain C shims for each — and those shims would themselves be compiled as C++.
  We would be writing C++ anyway, just badly and at the boundary.
- **No RAII.** In a codebase with this much resource ownership, every parser becomes a ladder
  of `goto cleanup`. This is a *known* generator of leaks, double-frees and use-after-free —
  precisely the defect class that TB-1 says is our highest technical risk.
- No type-safe generic containers. Either macro-based containers (unreadable, poor diagnostics)
  or `void*` containers (unsafe).
- Error handling is even more verbose than the C++ subset, which pushes people toward skipping
  checks.
- 150–250 kLOC of C with manual resource management, written under schedule pressure, will have
  memory-safety bugs. The question is only how many.

**Verdict: rejected.** The C-only argument is strongest when the dependency set is C and the
codebase is small. Neither holds here.

### B — C++20

**For:** concepts (far better template diagnostics), ranges, `std::format`, designated
initialisers, three-way comparison, coroutines (attractive for the job system).

**Against:**
- Android NDK r26's libc++ has incomplete C++20 library support; `std::format` and ranges
  availability across our toolchain matrix (NDK, Xcode, GCC 12, Emscripten) is inconsistent.
- Modules are not usable in practice across this toolchain set.
- Coroutines with `-fno-exceptions` are workable but awkward, and the job system is
  callback-based across a C ABI anyway, so the benefit is small.
- We would be debugging toolchain differences rather than building the product.

**Verdict: rejected for now.** Revisit at the Phase 12 boundary, when the NDK and Xcode
baselines have moved. The subset is chosen so that a later move to C++20 is additive.

### C — Rust core with a C ABI

**For:**
- Memory safety by construction — directly addresses our highest-severity risk class.
- Excellent tooling: `cargo`, `cargo-fuzz`, first-class cross-compilation.
- Fearless concurrency, which matters for the worker pool and sync engine.

**Against:**
- **The dependency set is the problem, not our code.** PDFium, Tesseract, OpenCV, SQLite and
  the image codecs are all C/C++. We would wrap every one in `unsafe` FFI. The memory-safety
  guarantee would cover our ~40 % of the code while the ~60 % that actually parses untrusted
  bytes stays exactly as unsafe as before. The benefit is real but much smaller than it first
  appears.
- Adds a second toolchain to every mobile build (`cargo-ndk`, iOS target triples, symbol and
  dSYM handling, reproducibility).
- Contradicts spec §2's explicit mandate.
- Rewriting PDF/OCR/imaging in pure Rust is a multi-year detour with worse libraries.

**Verdict: rejected as the core language.** But **not rejected as a tool**: Rust is a live
candidate for two narrow components where the C/C++ ecosystem is weak and the safety win is
concentrated — the text CRDT (`yrs`) and SVG rendering (`resvg`). See
`sync-architecture.md` §6. That decision is separate and flagged for sign-off.

### D — Mixed by module (C for parsers, C++ for orchestration)

**For:** parsers are where memory safety matters most, so write them in the language with the
fewest hidden behaviours; use C++ where abstraction pays.

**Against:**
- Inverts the actual argument. Parsers are exactly where RAII and bounds-checked views help
  most; orchestration code has the fewest resources to leak.
- Two idioms, two review standards, two sets of conventions in one codebase.
- The boundary between "parser" and "orchestration" is not crisp, and the line would be
  relitigated in every PR.

**Verdict: rejected.**

## Consequences

### Positive
- RAII eliminates the largest single class of C resource bugs by construction.
- C++ dependencies (PDFium, Tesseract, OpenCV) integrate directly, with no shim layer to write,
  test, fuzz and maintain.
- Type-safe containers and `string_view`/`span` remove a whole category of buffer-length errors.
- The C99 ABI keeps the Kotlin/Swift/WASM boundary simple and stable, and makes ABI-diffing a
  mechanical check.
- `-fno-exceptions -fno-rtti -fvisibility=hidden` keeps binary size close to C.
- Sanitizers, libFuzzer and clang-tidy all work well on this subset.

### Negative
- The subset must be **enforced**, not merely documented. Unenforced style rules erode.
  Mitigation: clang-tidy configuration, compiler flags, and a CI check for forbidden constructs.
- Compile times are longer than C. Mitigation: `ccache`, unity builds for third-party, careful
  header hygiene, explicit template instantiation in hot headers.
- New engineers must learn *our* subset, not general C++. Mitigation: a short written guide in
  `contributing.md` §5 and review enforcement.
- Debugging across the C ABI shows two stacks. Mitigation: the error chain in `uapp_error_t`
  carries causes rather than a flattened string.
- `-fno-exceptions` means allocation failure must be handled explicitly at every site.
  Mitigation: arena allocation for parse jobs (which has a single, checked failure point) and
  factory functions returning null.

### Neutral
- Binary size is expected within ~10 % of an equivalent C implementation, given the flag set.
  **To be verified in Phase 1** and reported against NFR-4.

## Enforcement

1. Compiler flags: `-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow
   -Wconversion -Werror -fvisibility=hidden`.
2. `.clang-tidy` with `bugprone-*`, `cert-*`, `cppcoreguidelines-*`, `concurrency-*`, plus
   custom checks banning `new`/`delete`, `dynamic_cast`, `<iostream>`, and raw owning pointers.
3. A CI job compiling `libuapp_capi` headers standalone with a **C compiler** at
   `-std=c99 -Wall -Wextra -Wpedantic`, proving the boundary is genuinely C.
4. An ABI-diff job asserting the exported symbol set changes only additively within a major.
5. Code review: the subset is a review criterion, listed in `contributing.md`.

## Revisit criteria

Reopen this ADR if any of the following becomes true:

- The NDK and Xcode baselines both support C++20 library features we want (→ consider C++20).
- Swift's C++ interop becomes stable and widely deployed enough to remove the C-ABI constraint
  (→ the binding layer could shrink, though the ABI stability argument would remain).
- Memory-safety defects in our own C++ (as opposed to vendored dependencies) exceed a
  meaningful rate in fuzzing and production — say, more than 3 exploitable findings per
  release train (→ reconsider Rust for the highest-risk modules specifically).
- A permissively-licensed, mature, pure-Rust PDF renderer and OCR engine emerge (→ the Rust
  argument changes materially, because the dependency objection disappears).
