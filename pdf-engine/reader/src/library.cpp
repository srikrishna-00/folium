// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/reader/library.cpp — the only place FPDF_InitLibrary and
// FPDF_DestroyLibrary are ever called.
//
// See document.h for the full rationale. In short: those two functions move
// PROCESS-GLOBAL state, so exactly one component may own the decision, and a
// grep for "FPDF_InitLibrary" must return this file and nothing else. Any
// second call site would be a second lifecycle policy.

#include "uapp/pdf/document.h"

#include <mutex>

#include "fpdfview.h"

namespace uapp::pdf {
namespace {

// A function-local static, not a namespace-scope object: this is constructed
// on first use, so there is no static-initialisation-order dependency between
// this translation unit and whatever calls into it first.
std::mutex& lifecycle_mutex() {
    static std::mutex m;
    return m;
}

// Guarded by lifecycle_mutex(). Plain int rather than std::atomic on purpose:
// an atomic counter would make the *count* thread-safe while leaving the
// init/destroy calls racy, which is the dangerous half. The mutex covers
// both, and this path runs once per document open — it is nowhere near a hot
// loop, so there is nothing to win by being clever.
int g_refcount = 0;

void init_pdfium_locked() {
    // Version 2 is what fpdfview.h documents as current and required; the
    // later versions are marked Experimental and gate on V8/Skia/Brotli
    // features this build deliberately does not have (pdf_enable_v8 = false,
    // AGG rasteriser — see third_party/pdfium/VERSION). Asking for a
    // renderer or font backend that is not compiled in is documented to
    // "fail with an immediate crash", so the conservative version is the
    // correct one, not merely the easy one.
    FPDF_LIBRARY_CONFIG config{};
    config.version          = 2;
    config.m_pUserFontPaths = nullptr;  // platform default font discovery
    config.m_pIsolate       = nullptr;  // no V8 in this build
    config.m_v8EmbedderSlot = 0;

    FPDF_InitLibraryWithConfig(&config);
}

}  // namespace

Library::Library() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex());
    if (g_refcount == 0) {
        init_pdfium_locked();
    }
    ++g_refcount;
    holds_ = true;
}

void Library::release() noexcept {
    if (!holds_) { return; }
    holds_ = false;

    const std::lock_guard<std::mutex> lock(lifecycle_mutex());
    // Defensive: a refcount that has gone wrong must not be turned into a
    // FPDF_DestroyLibrary() on an uninitialised library, which crashes
    // inside PDFium with no useful stack.
    if (g_refcount <= 0) {
        g_refcount = 0;
        return;
    }
    --g_refcount;
    if (g_refcount == 0) {
        FPDF_DestroyLibrary();
    }
}

Library::~Library() { release(); }

Library::Library(const Library& other) noexcept {
    if (other.holds_) {
        const std::lock_guard<std::mutex> lock(lifecycle_mutex());
        // Copying an existing reference can never be the 0 -> 1 transition,
        // so no init here; asserting that by construction is why this is not
        // written as `*this = Library()`.
        ++g_refcount;
        holds_ = true;
    }
}

Library& Library::operator=(const Library& other) noexcept {
    if (this == &other) { return *this; }
    // Acquire before release, so that assigning one live reference to
    // another cannot momentarily drop the global count to zero and tear
    // PDFium down between two statements.
    if (other.holds_) {
        const std::lock_guard<std::mutex> lock(lifecycle_mutex());
        ++g_refcount;
    }
    const bool was_holding = holds_;
    holds_ = other.holds_;
    if (was_holding) {
        const std::lock_guard<std::mutex> lock(lifecycle_mutex());
        if (g_refcount > 0) {
            --g_refcount;
            if (g_refcount == 0) { FPDF_DestroyLibrary(); }
        }
    }
    return *this;
}

Library::Library(Library&& other) noexcept : holds_(other.holds_) { other.holds_ = false; }

Library& Library::operator=(Library&& other) noexcept {
    if (this == &other) { return *this; }
    const bool incoming = other.holds_;
    other.holds_        = false;
    release();
    holds_ = incoming;
    return *this;
}

int Library::reference_count() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex());
    return g_refcount;
}

}  // namespace uapp::pdf
