// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/reader/document.h — read-only PDF: open, count, measure, render.
//
// SCOPE, STATED BEFORE ANYTHING ELSE
// ---------------------------------------------------------------------------
// Spec section 11 lists ~35 PDF features. This header implements FOUR of them,
// the ones the viewer cannot start without:
//
//     open a document from a buffer, count its pages, measure a page,
//     render a page into a buffer the CALLER owns.
//
// Everything else in section 11 — navigation state, search, thumbnails, merge,
// split, extract, reorder, delete, rotate, crop, compress, metadata,
// watermarks, passwords/encryption (writing), annotation, highlight,
// underline, strikethrough, drawing, shapes, text boxes, stamps, forms, form
// filling, redaction, digital signatures, PDF creation, image-to-PDF,
// OCR-to-PDF, PDF-to-image as a file-producing operation — is **NOT
// IMPLEMENTED**. Not stubbed, not partially present: absent. `docs/changelog.md`
// says so too, so this cannot quietly become "PDF support: done".
//
// The dependency underneath is also a bootstrap. `third_party/pdfium` holds a
// PREBUILT PDFium pinned by SHA-256, not the hermetic from-source build
// `docs/spikes/SP-1-findings.md` §5 requires before shipping. Nothing in this
// header depends on which of the two supplies libpdfium, which is the point of
// getting the wrapper written now.
//
// DESIGN RULES THIS HEADER OBEYS, AND WHY
// ---------------------------------------------------------------------------
//  1. **Buffer first, path never.** There is no `open_path`. The file engine
//     made the same choice (`detect_buffer` is the primary entry point, the
//     path overload is a convenience over it) for the same reason: on mobile
//     the bytes arrive from a document picker, a share sheet, a network
//     response or an encrypted blob store, and a path-only API forces a
//     temp-file round trip that puts plaintext on disk. If a path API is ever
//     wanted, it belongs on top of this, not underneath it.
//
//  2. **The caller owns the pixels.** `render_page` writes into memory the
//     caller supplies and never allocates a bitmap to hand back.
//     `docs/spikes/SP-5-findings.md` F-1/F-2 is explicit that the bitmap
//     budget must be met *structurally* rather than temporally: an
//     implementation that allocates-and-copies can still come in under 1 ms
//     and is one refactor away from not. PDFium's `FPDFBitmap_CreateEx()`
//     wraps caller memory instead of allocating, so on Android the caller's
//     buffer is the pixel array behind `AndroidBitmap_lockPixels()`, and on
//     iOS it is `CGBitmapContextGetData()`. Neither platform copies. That
//     path only exists if the C++ layer refuses to own the buffer, so it
//     refuses here.
//
//  3. **The caller also owns the PDF bytes, and must outlive the Document.**
//     `FPDF_LoadMemDocument()` does not copy its input; PDFium reads from the
//     caller's memory lazily for the document's whole life. Copying it here
//     to make the lifetime easier would double peak memory for a 100 MB PDF
//     (`file-format-support.md` lists 100 MB+ PDFs as supported for view), so
//     the contract is stated instead of paid for. This is the one sharp edge
//     in this header and it is repeated on `open_buffer`.
//
//  4. **`uapp::Status` everywhere, `[[nodiscard]]` everywhere.** ADR-0001:
//     no exceptions. Every fallible call returns a Status; nothing throws;
//     nothing returns a sentinel the caller can ignore.
//
//  5. **Malformed input is expected input.** Spec section 29 names the PDF
//     parser as a fuzz target, and `docs/risk-register.md` treats it as the
//     highest-risk parser in the product. Every entry point validates before
//     it calls PDFium, and PDFium's own failures map onto Status rather than
//     onto a crash. No fuzz target exists for this module yet — that gap is
//     recorded in the changelog, not papered over.

#ifndef UAPP_PDF_DOCUMENT_H
#define UAPP_PDF_DOCUMENT_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "uapp/status.h"

namespace uapp::pdf {

/// Upper bound on a render dimension, in pixels, per side.
///
/// Not a performance tuning knob — an overflow guard. A hostile or merely
/// silly caller asking for 2^20 x 2^20 must be refused by arithmetic we
/// control, before `width * height * 4` is computed anywhere. 20000 px is
/// well past any real display or print target (a 300 dpi A4 page is ~2480 x
/// 3508) and leaves `width * 4` and `stride * height` far inside a
/// std::size_t on every supported platform.
inline constexpr int kMaxRenderDimension = 20000;

/// Bytes per pixel of the one output format this module produces.
inline constexpr std::size_t kRenderBytesPerPixel = 4;

/// A page's intrinsic size in PDF user-space units (points, 1/72 inch),
/// which is what the PDF's MediaBox carries. Deliberately NOT pixels: the
/// pixel size of a render is the caller's choice, and conflating the two is
/// how a viewer ends up with a hard-coded DPI.
struct PageSize final {
    double width_points  = 0.0;
    double height_points = 0.0;
};

/// A caller-owned destination for one rendered page.
///
/// RGBA8888, 8 bits per channel, byte order R, G, B, A at ascending
/// addresses — i.e. the memory layout Android's `ARGB_8888` and Skia's
/// `kRGBA_8888_SkColorType` use, not a 32-bit integer whose interpretation
/// depends on endianness. PDFium is asked for BGRA and told to reverse the
/// byte order, which is the documented way to get exactly this.
///
/// The buffer is NOT cleared by the caller's contract but IS cleared by ours:
/// `render_page` fills it with opaque white before drawing, because a PDF
/// page has no background of its own and compositing page content onto
/// uninitialised memory is how "sometimes there is garbage behind the text"
/// bugs happen.
struct RenderTarget final {
    /// First byte of the first row. Must be non-null.
    std::uint8_t* pixels = nullptr;

    /// Bytes addressable from `pixels`. Checked against the requested
    /// geometry; the check is the whole reason this field exists, so passing
    /// a made-up value here defeats it.
    std::size_t capacity_bytes = 0;

    /// Requested raster size in pixels. Both must be in [1, kMaxRenderDimension].
    int width  = 0;
    int height = 0;

    /// Distance between the starts of consecutive rows, in bytes. 0 means
    /// "tightly packed", i.e. width * 4. A caller rendering into a platform
    /// bitmap passes that bitmap's own stride (Android's `AndroidBitmapInfo::
    /// stride`, iOS's `CGBitmapContextGetBytesPerRow`), which is frequently
    /// padded — refusing padded strides would force the copy this whole
    /// design exists to avoid.
    ///
    /// Must be >= width * 4 and a MULTIPLE OF 4. The alignment requirement is
    /// not ours: PDFium's 32-bpp bitmaps assume 4-byte-aligned scanlines and
    /// abort the process on a CHECK if given anything else. Both platform
    /// strides above are always multiples of 4, so this constrains nobody
    /// real — but it is enforced, as a Status, rather than trusted.
    std::size_t stride_bytes = 0;

    /// The stride actually in effect (resolves the 0 == packed case).
    /// Returns 0 if `width` is not usable, so callers cannot get a plausible
    /// number out of a nonsense target.
    [[nodiscard]] std::size_t effective_stride() const noexcept;

    /// Validate the geometry against `capacity_bytes`, overflow-safely.
    /// Returns Ok, PdfRenderSizeInvalid or PdfBufferTooSmall. Public because
    /// a caller sizing a bitmap pool wants the same answer without a
    /// Document in hand.
    [[nodiscard]] Status validate() const noexcept;
};

/// Minimum `capacity_bytes` for a given geometry, or 0 if the geometry is
/// unusable (non-positive, over kMaxRenderDimension, stride below the row
/// width, or arithmetically overflowing). 0 always means "do not allocate
/// this", never "no bytes needed".
[[nodiscard]] std::size_t required_buffer_bytes(int width, int height,
                                                std::size_t stride_bytes) noexcept;

// ---------------------------------------------------------------------------
// Library — PDFium's process-global lifecycle, made safe to use more than once
// ---------------------------------------------------------------------------
//
// `FPDF_InitLibrary()` / `FPDF_DestroyLibrary()` set up and tear down
// PROCESS-GLOBAL state (font caches, the codec modules, the global allocator
// hooks). They are not reentrant, they are not thread-safe against each
// other, and calling a PDFium function outside an init/destroy pair is
// undefined behaviour rather than an error you get told about.
//
// A test binary runs many cases in one process, so "initialise once in main()"
// is not good enough: it makes every test depend on a global that some other
// test could tear down, and it leaves the library initialised for the life of
// an app that opened one PDF an hour ago. Upstream PDFium's own embedder tests
// init and destroy per test, so the cycle is a supported path.
//
// So: a reference count, guarded by a mutex. The first `Library` in the
// process initialises PDFium, the last one destroys it, and any number may
// exist in between. `Document` holds one, so a caller that never mentions
// `Library` still gets correct lifecycle handling; a caller that opens
// documents in a loop holds one explicitly to keep PDFium up across the whole
// loop instead of paying init/destroy each time.
//
// THE ONE RULE: every Document must be destroyed before the last Library
// goes away. Holding a Document in a static and letting it die during static
// destruction ordering is the way to break that, so do not do it. Document
// keeps its own Library, so the ordinary stack-scoped case is automatic.
class Library final {
public:
    /// Acquire a reference, initialising PDFium if this is the first one.
    Library() noexcept;
    ~Library();

    Library(const Library& other) noexcept;
    Library& operator=(const Library& other) noexcept;
    Library(Library&& other) noexcept;
    Library& operator=(Library&& other) noexcept;

    /// True iff this object currently holds a reference. False only after a
    /// move-from; used by Document to avoid double-releasing.
    [[nodiscard]] bool holds_reference() const noexcept { return holds_; }

    /// Live reference count. For tests and diagnostics — asserting that the
    /// count returns to zero is how "we do not leak PDFium initialisations"
    /// becomes a checkable property instead of a claim.
    [[nodiscard]] static int reference_count() noexcept;

private:
    void release() noexcept;

    bool holds_ = false;
};

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------
//
// Move-only RAII over `FPDF_DOCUMENT`. Copying would mean two owners of one
// PDFium handle and therefore a double `FPDF_CloseDocument()`, so it is
// deleted rather than reference-counted: a PDF document is not a value.
class Document final {
public:
    Document() noexcept = default;
    ~Document();

    Document(const Document&)            = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;

    /// Open a PDF held entirely in memory.
    ///
    /// **`data` must stay valid, unmodified and at the same address until
    /// this Document is closed or destroyed.** PDFium parses lazily out of
    /// the caller's buffer and never copies it. Reading a PDF whose bytes
    /// have been freed is a use-after-free inside PDFium, and no amount of
    /// checking here can detect it, which is why it is stated this loudly.
    ///
    /// `password` may be empty for an unencrypted document. It is passed to
    /// PDFium as a NUL-terminated byte string, so an embedded NUL truncates
    /// it — a password containing NUL is rejected rather than silently cut.
    ///
    /// `out` is overwritten on success and left closed on failure; a failed
    /// open never leaves a half-open Document behind.
    ///
    /// Returns:
    ///   Ok
    ///   InvalidArgument         — null `data`, or a NUL inside `password`
    ///   FileEmpty               — `size` is 0
    ///   PdfMalformed            — not a PDF, or damaged past recovery
    ///   PdfPasswordRequired     — encrypted, and the password did not open it
    ///   PdfSecurityUnsupported  — a security handler PDFium will not open
    [[nodiscard]] static Status open_buffer(const std::uint8_t* data, std::size_t size,
                                            std::string_view password, Document& out);

    /// Same, for the overwhelmingly common unencrypted case.
    [[nodiscard]] static Status open_buffer(const std::uint8_t* data, std::size_t size,
                                            Document& out);

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

    /// Pages in the document, or 0 if none is open. A valid PDF always has
    /// at least one page, so 0 from an open Document means PDFium could not
    /// read the page tree — `page_size`/`render_page` will then report the
    /// specific failure.
    [[nodiscard]] int page_count() const noexcept;

    /// Intrinsic size of page `index` in points. Does not rasterise.
    ///
    /// Returns Ok, PdfNotOpen, PdfPageNotFound, or PdfPageLoadFailed.
    [[nodiscard]] Status page_size(int index, PageSize& out) const;

    /// Rasterise page `index` into `target`, scaled to fill
    /// `target.width` x `target.height` exactly.
    ///
    /// The aspect ratio is NOT preserved: this renders the page into the box
    /// it is given, because the caller — who already knows the page size from
    /// `page_size()` — is the only party that knows whether it wants
    /// letterboxing, cropping or a stretch. Deciding for it here would be a
    /// policy baked into a rasteriser.
    ///
    /// The buffer is filled with opaque white first, then the page is drawn
    /// over it. On any failure the buffer's contents are unspecified but
    /// every written byte stays inside `capacity_bytes`.
    ///
    /// Returns:
    ///   Ok
    ///   PdfNotOpen | PdfPageNotFound | PdfPageLoadFailed
    ///   PdfRenderSizeInvalid  — geometry is non-positive/oversized/overflowing
    ///   PdfBufferTooSmall     — capacity_bytes cannot hold that geometry
    ///   PdfRenderFailed       — PDFium would not accept the target
    [[nodiscard]] Status render_page(int index, const RenderTarget& target) const;

    /// Close early. Idempotent; safe on a default-constructed Document.
    void close() noexcept;

private:
    Library library_{};
    void*   handle_ = nullptr;  ///< FPDF_DOCUMENT. void* keeps PDFium's
                                ///< headers out of this one, so nothing that
                                ///< includes it inherits a PDFium include path.
};

}  // namespace uapp::pdf

#endif  // UAPP_PDF_DOCUMENT_H
