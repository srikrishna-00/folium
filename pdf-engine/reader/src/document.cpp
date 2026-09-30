// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/reader/document.cpp — the PDFium C API, wrapped.
//
// This is the only first-party translation unit that includes fpdfview.h
// besides library.cpp, and `document.h` exposes the handle as `void*`
// precisely so it stays that way: nothing that merely wants to open a PDF
// should inherit PDFium's include path, and the day the underlying library
// changes (SP-1's hermetic build, or the MuPDF-commercial fallback R-12 keeps
// alive) the blast radius is this directory.

#include "uapp/pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "fpdfview.h"

namespace uapp::pdf {
namespace {

FPDF_DOCUMENT as_document(void* handle) noexcept { return static_cast<FPDF_DOCUMENT>(handle); }

/// Translate PDFium's process-global last-error into a Status, for a failed
/// DOCUMENT LOAD and for nothing else.
///
/// MUST be called immediately after the failing PDFium call: the error is a
/// global that the next call overwrites.
///
/// "And for nothing else" is a correctness constraint, not a style rule, and
/// it was learned the hard way — see the comment on the page-level failure in
/// `page_size` below. `FPDF_LoadMemDocument64` sets the last error on every
/// failure path, so reading it there is meaningful. The page-level entry
/// points make no such promise, and reading a global that nobody wrote yields
/// whatever the *previous* call left behind.
///
/// Two arms below (`FPDF_ERR_SUCCESS` -> Internal, `FPDF_ERR_PAGE` ->
/// PdfPageLoadFailed) produce a Status that is not in `open_buffer`'s
/// documented return set. Both are believed unreachable from a document load,
/// and that belief is checked rather than asserted: the oracle in
/// `tests/fuzz/fuzz_pdf_reader.cpp` holds every `open_buffer` return against
/// the documented set on every input, so if either becomes reachable the
/// fuzzer says so instead of a caller finding out in production.
Status status_from_pdfium_error() noexcept {
    switch (FPDF_GetLastError()) {
        case FPDF_ERR_SUCCESS:
            // PDFium said "failed" and "no error". That is PDFium
            // contradicting itself, not a caller mistake, so it is not
            // reported as a malformed file.
            return Status::Internal;
        case FPDF_ERR_FILE:
            // Reachable for a memory document: PDFium uses ERR_FILE for
            // "could not read the requested range", which for an in-memory
            // document means the buffer ended where the file said it would
            // not — a truncated PDF.
            return Status::PdfMalformed;
        case FPDF_ERR_FORMAT:
            return Status::PdfMalformed;
        case FPDF_ERR_PASSWORD:
            return Status::PdfPasswordRequired;
        case FPDF_ERR_SECURITY:
            return Status::PdfSecurityUnsupported;
        case FPDF_ERR_PAGE:
            return Status::PdfPageLoadFailed;
        default:
            // FPDF_ERR_UNKNOWN and anything a future PDFium adds. Reported
            // as malformed rather than Internal: the overwhelmingly likely
            // cause of an unclassified failure while parsing attacker-
            // supplied bytes is the bytes, and calling it an internal defect
            // would send whoever reads the log to the wrong place.
            return Status::PdfMalformed;
    }
}

/// RAII for FPDF_PAGE. Every early return below would otherwise have to
/// remember FPDF_ClosePage; `render_page` has five of them.
class ScopedPage final {
public:
    ScopedPage(FPDF_DOCUMENT doc, int index) noexcept : page_(FPDF_LoadPage(doc, index)) {}
    ~ScopedPage() {
        if (page_ != nullptr) { FPDF_ClosePage(page_); }
    }

    ScopedPage(const ScopedPage&)            = delete;
    ScopedPage& operator=(const ScopedPage&) = delete;
    ScopedPage(ScopedPage&&)                 = delete;
    ScopedPage& operator=(ScopedPage&&)      = delete;

    [[nodiscard]] FPDF_PAGE get() const noexcept { return page_; }
    [[nodiscard]] bool      valid() const noexcept { return page_ != nullptr; }

private:
    FPDF_PAGE page_;
};

/// RAII for FPDF_BITMAP. Destroying the bitmap does NOT free the pixels when
/// the bitmap wraps an external buffer (fpdfview.h: "FPDFBitmap_Destroy()
/// will not destroy the buffer") — which is the entire basis of the
/// caller-owns-the-pixels contract, so it is worth stating where it is relied
/// on rather than only in the header.
class ScopedBitmap final {
public:
    ScopedBitmap(int width, int height, void* first_scan, int stride) noexcept
        : bitmap_(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, first_scan, stride)) {}
    ~ScopedBitmap() {
        if (bitmap_ != nullptr) { FPDFBitmap_Destroy(bitmap_); }
    }

    ScopedBitmap(const ScopedBitmap&)            = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
    ScopedBitmap(ScopedBitmap&&)                 = delete;
    ScopedBitmap& operator=(ScopedBitmap&&)      = delete;

    [[nodiscard]] FPDF_BITMAP get() const noexcept { return bitmap_; }
    [[nodiscard]] bool        valid() const noexcept { return bitmap_ != nullptr; }

private:
    FPDF_BITMAP bitmap_;
};

/// Opaque white, as FPDFBitmap_FillRect takes it (0xAARRGGBB).
///
/// White is also the one fill value for which the BGRA-vs-RGBA question does
/// not arise: FillRect writes in the bitmap's native BGRA order while the
/// page is drawn with FPDF_REVERSE_BYTE_ORDER, and any asymmetric colour
/// would come out channel-swapped relative to the content. If a configurable
/// background is ever wanted, that mismatch is the thing to fix first.
constexpr unsigned long kOpaqueWhite = 0xFFFFFFFFUL;

}  // namespace

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

Document::~Document() { close(); }

Document::Document(Document&& other) noexcept
    : library_(std::move(other.library_)), handle_(other.handle_) {
    other.handle_ = nullptr;
}

Document& Document::operator=(Document&& other) noexcept {
    if (this == &other) { return *this; }
    // Close BEFORE taking the incoming library reference away from `other`:
    // FPDF_CloseDocument must run while PDFium is still initialised, and our
    // own reference is what guarantees that.
    close();
    handle_       = other.handle_;
    other.handle_ = nullptr;
    library_      = std::move(other.library_);
    return *this;
}

void Document::close() noexcept {
    if (handle_ != nullptr) {
        FPDF_CloseDocument(as_document(handle_));
        handle_ = nullptr;
    }
}

Status Document::open_buffer(const std::uint8_t* data, std::size_t size,
                             std::string_view password, Document& out) {
    out.close();

    if (size == 0) {
        // Checked before the null check so that (nullptr, 0) — the natural
        // spelling of "no data" — gets the specific answer rather than the
        // generic one.
        return Status::FileEmpty;
    }
    if (data == nullptr) { return Status::InvalidArgument; }

    // PDFium takes the password as a NUL-terminated byte string, so a NUL
    // inside the caller's view would silently truncate it and we would then
    // report "wrong password" for a password we never fully passed. Refuse.
    if (password.find('\0') != std::string_view::npos) { return Status::InvalidArgument; }

    const std::string password_z(password);
    const char* const password_arg = password.empty() ? nullptr : password_z.c_str();

    // A Library reference is taken BEFORE the first PDFium call and held by
    // the local until it is moved into `out`. If the load fails, the local
    // dies here and the refcount returns to where it was — a failed open
    // must not leave PDFium initialised forever.
    Library library;

    // FPDF_LoadMemDocument64 rather than FPDF_LoadMemDocument: the latter
    // takes an `int` size, so a >2 GB buffer would be truncated or go
    // negative. file-format-support.md lists 100 MB+ PDFs as supported and
    // nothing bounds the input at 2 GB, so the 64-bit entry point is the
    // correct one even though today's tests never reach it.
    //
    // This does NOT copy `data`; see the contract on open_buffer().
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(data, size, password_arg);
    if (doc == nullptr) { return status_from_pdfium_error(); }

    out.library_ = std::move(library);
    out.handle_  = doc;
    return Status::Ok;
}

Status Document::open_buffer(const std::uint8_t* data, std::size_t size, Document& out) {
    return open_buffer(data, size, std::string_view(), out);
}

int Document::page_count() const noexcept {
    if (handle_ == nullptr) { return 0; }
    const int count = FPDF_GetPageCount(as_document(handle_));
    // PDFium returns 0 (not a negative) for an unreadable page tree, but the
    // return type is signed and this value is about to be compared against
    // caller indices, so clamp rather than trust.
    return count > 0 ? count : 0;
}

Status Document::page_size(int index, PageSize& out) const {
    out = PageSize{};

    if (handle_ == nullptr) { return Status::PdfNotOpen; }
    if (index < 0 || index >= page_count()) { return Status::PdfPageNotFound; }

    // FPDF_GetPageSizeByIndexF does not load the page's content streams, so
    // measuring a page is cheap enough to call while laying out a scroll
    // view over a 1000-page document. Loading the page just to read its
    // MediaBox would make that layout pass parse the whole file.
    FS_SIZEF size{};
    if (FPDF_GetPageSizeByIndexF(as_document(handle_), index, &size) == 0) {
        // PdfPageLoadFailed unconditionally, NOT status_from_pdfium_error().
        //
        // This used to consult FPDF_GetLastError(), and that was a real bug —
        // found by tests/fuzz/fuzz_pdf_reader on a PDF whose /Kids array
        // names an object that does not exist. `FPDF_GetPageSizeByIndexF`
        // does not set the last error when it cannot resolve a page, so the
        // global still held FPDF_ERR_SUCCESS from the document load that
        // succeeded a moment earlier, and the mapper turned "no error" into
        // Status::Internal — a code status.h defines as "a bug in us, not in
        // the caller's input" for a failure caused entirely by the caller's
        // input. Attacker-supplied bytes could therefore make this module
        // report an internal defect, which is both a lie to whoever reads the
        // log and a way to poison an error-rate alert. It also put a status
        // outside this function's documented set (Ok / PdfNotOpen /
        // PdfPageNotFound / PdfPageLoadFailed) in front of callers switching
        // on it.
        //
        // Status::PdfPageLoadFailed is 704, "index in range, but the page
        // object is unusable" — which is exactly and only what a failure here
        // means. Nothing is lost by not reading the global, because nothing
        // wrote it.
        return Status::PdfPageLoadFailed;
    }

    out.width_points  = static_cast<double>(size.width);
    out.height_points = static_cast<double>(size.height);
    return Status::Ok;
}

Status Document::render_page(int index, const RenderTarget& target) const {
    if (handle_ == nullptr) { return Status::PdfNotOpen; }

    // Geometry first, before anything is loaded: a bad target is a caller
    // bug and should not cost a page parse to discover.
    const Status geometry = target.validate();
    if (!is_ok(geometry)) { return geometry; }

    // FPDFBitmap_CreateEx takes the stride as an `int`. validate() has
    // already bounded width at kMaxRenderDimension, but the stride comes
    // straight from the caller (it is a platform bitmap's stride) and is a
    // std::size_t, so it is the one value that can still exceed INT_MAX.
    const std::size_t stride = target.effective_stride();
    if (stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status::PdfRenderSizeInvalid;
    }

    if (index < 0 || index >= page_count()) { return Status::PdfPageNotFound; }

    const ScopedPage page(as_document(handle_), index);
    // Same reasoning, same fix as page_size(): FPDF_LoadPage does not set the
    // last error either, so consulting it here reported a stale document-load
    // outcome — Status::Internal in the case the fuzzer found. An in-range
    // index whose page will not load IS PdfPageLoadFailed, and that is in
    // render_page's documented set.
    if (!page.valid()) { return Status::PdfPageLoadFailed; }

    // THE ZERO-COPY POINT (SP-5 F-2). `target.pixels` is the caller's memory;
    // CreateEx wraps it instead of allocating, FPDF_RenderPageBitmap writes
    // through it, and Destroy leaves it alone. Nothing between here and the
    // caller's bitmap is copied, and there is no intermediate buffer to be
    // "optimised away" later — there is none to begin with.
    const ScopedBitmap bitmap(target.width, target.height, target.pixels,
                              static_cast<int>(stride));
    if (!bitmap.valid()) { return Status::PdfRenderFailed; }

    // A PDF page has no background; without this the page composites onto
    // whatever the caller's buffer happened to contain.
    if (FPDFBitmap_FillRect(bitmap.get(), 0, 0, target.width, target.height, kOpaqueWhite) == 0) {
        return Status::PdfRenderFailed;
    }

    // FPDF_REVERSE_BYTE_ORDER turns PDFium's native BGRA into the RGBA8888
    // byte order document.h promises. It is the documented way to get that
    // layout, and it costs nothing — PDFium picks a different output path,
    // it does not swap afterwards.
    //
    // FPDF_ANNOT is deliberately NOT set: annotations are a section 11
    // feature this slice does not implement, and rendering them while the
    // rest of the annotation surface is absent would make the viewer's
    // behaviour inconsistent with its own capabilities.
    FPDF_RenderPageBitmap(bitmap.get(), page.get(),
                          /*start_x=*/0, /*start_y=*/0,
                          /*size_x=*/target.width, /*size_y=*/target.height,
                          /*rotate=*/0,
                          /*flags=*/FPDF_REVERSE_BYTE_ORDER);

    // FPDF_RenderPageBitmap returns void. There is no success signal to
    // check, so a page whose content stream is broken renders as far as
    // PDFium got and reports Ok. That is PDFium's contract, not a shortcut
    // here, and pretending otherwise by inventing a failure condition would
    // be worse. Callers wanting "did it look right" need a pixel comparison,
    // which is what the fixture-corpus reference-image test in
    // testing-strategy.md §4 is for and which does not exist yet.
    return Status::Ok;
}

}  // namespace uapp::pdf
