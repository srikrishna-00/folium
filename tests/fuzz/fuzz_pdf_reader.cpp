// SPDX-License-Identifier: Apache-2.0
//
// tests/fuzz/fuzz_pdf_reader.cpp — the PDF target testing-strategy.md §6.1
// names FIRST in its required list ("Largest attack surface", P0), and which
// docs/risk-register.md treats as the highest-risk parser in the product.
//
// It stands in for §6.1's `fuzz_pdf_parse` AND `fuzz_pdf_render`, because
// `pdf-engine/reader` is one library with one entry point and splitting it
// into two harnesses would mean two processes re-parsing the same bytes to
// reach the second half of the same call chain. §6.1's rationale for listing
// them separately — "rendering is a separate code path from parsing" — is
// honoured by *exercising both in every iteration*, not by forking the
// binary. `fuzz_pdf_content_stream` stays unbuilt: there is no first-party
// content-stream entry point to call.
//
// ===========================================================================
// WHAT IS BEING FUZZED, AND WHY THIS ENTRY POINT
// ===========================================================================
// `Document::open_buffer` is the untrusted-upload shape, for exactly the
// reason `detect_buffer` is in fuzz_file_detection.cpp: raw bytes, no path,
// no filesystem, no provenance. It is also the ONLY door into this module —
// document.h design rule 1 is "buffer first, path never", so there is no
// second entry point to leave unfuzzed.
//
// The input byte string IS the PDF file, verbatim. No length prefix, no
// framing, no embedded control fields — the same constraint fuzz_file_
// detection.cpp adopted and for the same three reasons: every corpus entry
// under tests/fixtures/corpus/fuzz_pdf_reader/ is a real (if often hostile)
// PDF, a crash reproducer is directly usable as a regression fixture, and
// nobody can later mistake a harness framing bug for a parser bug.
//
// The two things a PDF's bytes cannot carry are the PASSWORD and the RENDER
// GEOMETRY. Both are therefore derived deterministically from the input's own
// first byte, as the filename is in fuzz_file_detection.cpp: deterministic so
// libFuzzer's reproduction guarantee holds, and derived rather than embedded
// so the corpus stays a directory of PDFs.
//
// ===========================================================================
// THE HONEST LIMIT OF THIS TARGET: PDFIUM IS NOT INSTRUMENTED
// ===========================================================================
// `third_party/pdfium` is a PREBUILT `libpdfium.dylib` (see its README, "What
// is still owed"). It carries no SanitizerCoverage and no ASan/UBSan
// instrumentation, and nothing this harness does can add any.
//
// Two consequences, stated here rather than discovered later:
//
//   1. libFuzzer's coverage feedback comes from the WRAPPER (document.cpp,
//      render_target.cpp, library.cpp) and from nothing inside the parser.
//      The mutator is therefore close to blind past the first branch that
//      matters, which is why the seed corpus carries hand-built adversarial
//      structures instead of trusting mutation to invent them.
//   2. ASan sees PDFium's heap through the malloc interceptor — so a
//      double-free, a use-after-free of a heap block, or an overflow caught
//      by an intercepted libc call IS reported — but a plain out-of-bounds
//      read inside uninstrumented PDFium code is NOT, because there is no
//      instrumented load to check. UBSan sees nothing in PDFium at all.
//
// What this target therefore genuinely covers is *our* layer: the status
// mapping, the lifecycle refcount, the bounds arithmetic, and — because the
// render destination is an EXACT-FIT heap allocation with ASan redzones on
// both sides — any write by PDFium outside the buffer `RenderTarget`
// authorised. That last one is the memory-safety property this module
// actually promises, and it is checkable today. The rest waits for the
// hermetic from-source PDFium build SP-1 asks for; the gap is recorded in
// docs/testing-strategy.md §6.4 rather than papered over.
//
// ===========================================================================
// THE ORACLE
// ===========================================================================
// Crashes are not the only bug. Every input is also held to the Status sets
// document.h states in prose — and ONLY to those. Inventing a stricter rule
// than the header promises would turn a harness opinion into a fake product
// defect, which is the trap fuzz_file_detection.cpp calls out and this file
// inherits. Notably NOT asserted: that a page has positive dimensions, that
// a recovered document has any particular page count, or that rendering a
// broken content stream fails — document.h promises none of those, and
// PDFium's xref-rebuilding recovery makes all three legitimately variable.
//
// A violation calls std::abort(), so libFuzzer records it exactly as it
// records a sanitizer fault. -fno-exceptions (ADR-0001) leaves no throwing
// alternative, and that is fine: an oracle failure IS a crash.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "uapp/pdf/document.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::pdf::Document;
using uapp::pdf::Library;
using uapp::pdf::PageSize;
using uapp::pdf::RenderTarget;

[[noreturn]] void oracle_failed(const char* rule, const char* detail) {
    std::fprintf(stderr,
                 "\n*** fuzz_pdf_reader ORACLE VIOLATION ***\n"
                 "  invariant: %s\n"
                 "  detail   : %s\n",
                 rule, detail == nullptr ? "(none)" : detail);
    std::fflush(stderr);
    std::abort();
}

#define ORACLE(cond, detail)                          \
    do {                                              \
        if (!(cond)) { oracle_failed(#cond, detail); } \
    } while (false)

/// document.h open_buffer(): "Returns: Ok | InvalidArgument | FileEmpty |
/// PdfMalformed | PdfPasswordRequired | PdfSecurityUnsupported". Anything
/// else is an undocumented outcome, which for a caller switching on Status
/// is a bug whether or not it crashes.
bool is_documented_open_status(Status s) noexcept {
    return s == Status::Ok || s == Status::InvalidArgument || s == Status::FileEmpty ||
           s == Status::PdfMalformed || s == Status::PdfPasswordRequired ||
           s == Status::PdfSecurityUnsupported;
}

/// document.h page_size(): "Returns Ok, PdfNotOpen, PdfPageNotFound, or
/// PdfPageLoadFailed."
bool is_documented_page_size_status(Status s) noexcept {
    return s == Status::Ok || s == Status::PdfNotOpen || s == Status::PdfPageNotFound ||
           s == Status::PdfPageLoadFailed;
}

/// document.h render_page(): "Returns: Ok | PdfNotOpen | PdfPageNotFound |
/// PdfPageLoadFailed | PdfRenderSizeInvalid | PdfBufferTooSmall |
/// PdfRenderFailed", plus InvalidArgument from RenderTarget::validate() for a
/// null `pixels`.
bool is_documented_render_status(Status s) noexcept {
    return s == Status::Ok || s == Status::PdfNotOpen || s == Status::PdfPageNotFound ||
           s == Status::PdfPageLoadFailed || s == Status::PdfRenderSizeInvalid ||
           s == Status::PdfBufferTooSmall || s == Status::PdfRenderFailed ||
           s == Status::InvalidArgument;
}

/// Passwords to try when a document says it is encrypted. The empty string is
/// not here — that is the first attempt, made by the no-password overload.
/// "" would also be indistinguishable from it, and the point of this table is
/// to reach `FPDF_LoadMemDocument64` with a non-null password argument and
/// therefore into PDFium's security-handler code, which the unencrypted path
/// never touches.
const char* const kPasswords[] = {
    "owner",
    "user",
    "0000",
    // A long one: the RC4/AES key-derivation paths pad or truncate at 32
    // bytes, so a password either side of that boundary exercises different
    // code inside the standard security handler.
    "0123456789abcdef0123456789abcdefX",
};
constexpr std::size_t kPasswordCount = sizeof(kPasswords) / sizeof(kPasswords[0]);

/// Render geometries. Small on purpose: rasterising is the slow half of this
/// target and a 2000x2000 raster would drop the iteration rate by three
/// orders of magnitude while testing the same bounds arithmetic. What varies
/// instead is the SHAPE, because that is what the code under test branches
/// on — a packed stride, a padded stride (the Android/iOS platform-bitmap
/// case from SP-5 F-2, and the one that a stride-ignoring renderer shears),
/// a single-pixel target, and a very wide, very short one.
struct Geometry final {
    int         width;
    int         height;
    std::size_t extra_row_padding;  // bytes added to width*4 to form the stride
};

constexpr Geometry kGeometries[] = {
    {64, 64, 0},
    {64, 64, 36},  // not a power of two, still 4-byte aligned as PDFium demands
    {1, 1, 0},
    {200, 100, 0},  // the fixture's own page size at 1 px/pt
    {97, 3, 8},     // odd width, near-degenerate height, padded
    {8, 8, 0},
};
constexpr std::size_t kGeometryCount = sizeof(kGeometries) / sizeof(kGeometries[0]);

/// Poison, not 0: zeroed memory makes "the renderer wrote nothing" and "the
/// renderer wrote transparent black" the same observation. Same value and
/// same reasoning as PixelBuffer in tests/pdf_engine/pdf_reader_test.cpp.
constexpr std::uint8_t kPoison = 0x5A;

/// How many pages get measured. A hostile PDF can claim a very large page
/// count cheaply, and walking all of it would turn every such input into a
/// libFuzzer timeout — a HARNESS-amplified hang, not a product one, and
/// §6.2's "timeout and OOM are findings" only means anything if the findings
/// are the product's. The last index is always probed as well (below), so the
/// count/index boundary is still covered on a large document.
constexpr int kMaxPagesMeasured = 48;

/// Measure pages and hold each answer to page_size()'s documented Status set.
void measure_pages(const Document& doc, int page_count) {
    const int limit = page_count < kMaxPagesMeasured ? page_count : kMaxPagesMeasured;

    for (int i = 0; i < limit; ++i) {
        PageSize     size;
        const Status status = doc.page_size(i, size);
        ORACLE(is_documented_page_size_status(status),
               "page_size returned a status outside its documented set");
        ORACLE(status != Status::PdfNotOpen, "page_size said not-open on an open Document");
        // The values themselves are deliberately NOT constrained. A PDF's
        // MediaBox is attacker-controlled and document.h promises only that
        // these are "the page's intrinsic size in points" — a zero, a
        // negative or an absurd extent is a property of the file, not a
        // defect here. They are read so the call cannot be optimised away.
        if (uapp::is_ok(status)) {
            volatile double sink = size.width_points + size.height_points;
            (void)sink;
        }
    }

    // The boundary, which IS a firm contract: "Returns Ok, PdfNotOpen,
    // PdfPageNotFound, or PdfPageLoadFailed" combined with page_size()'s
    // own `index < 0 || index >= page_count()` guard.
    PageSize out_of_range;
    ORACLE(doc.page_size(-1, out_of_range) == Status::PdfPageNotFound,
           "a negative page index was not PdfPageNotFound");
    ORACLE(doc.page_size(page_count, out_of_range) == Status::PdfPageNotFound,
           "page_size(page_count) was not PdfPageNotFound");

    // The last real page, even on a document too large to walk. Different
    // code from page 0 on any document with a page tree deeper than one node.
    if (page_count > kMaxPagesMeasured) {
        PageSize     last;
        const Status status = doc.page_size(page_count - 1, last);
        ORACLE(is_documented_page_size_status(status),
               "page_size on the last page returned a status outside its documented set");
    }
}

/// Rasterise page 0 into a real, caller-owned, EXACT-FIT heap buffer.
///
/// Exact-fit is the load-bearing detail. `required_buffer_bytes()` is asked
/// for the size and the vector is given precisely that, so the allocation is
/// flanked by ASan redzones at both ends with no slack in between: any write
/// PDFium makes past the geometry `RenderTarget` authorised lands in a
/// redzone and is reported, instead of landing in spare capacity nobody
/// looks at. Rounding the allocation up "for safety" would silently disable
/// the single memory-safety check this target can still make against an
/// uninstrumented PDFium.
void render_first_page(const Document& doc, int page_count, std::size_t selector) {
    const Geometry& geometry = kGeometries[selector % kGeometryCount];

    const std::size_t stride =
        static_cast<std::size_t>(geometry.width) * uapp::pdf::kRenderBytesPerPixel +
        geometry.extra_row_padding;
    const std::size_t needed =
        uapp::pdf::required_buffer_bytes(geometry.width, geometry.height, stride);

    // A harness self-check, not a product assertion: every entry in
    // kGeometries must be a geometry the module accepts, or this target
    // would spend its budget proving that render_page rejects bad targets —
    // which is a unit test's job (pdf.malformed), not a fuzzer's.
    ORACLE(needed != 0, "a hard-coded harness geometry was rejected as unusable");

    std::vector<std::uint8_t> pixels(needed, kPoison);

    RenderTarget target;
    target.pixels         = pixels.data();
    target.capacity_bytes = pixels.size();
    target.width          = geometry.width;
    target.height         = geometry.height;
    target.stride_bytes   = stride;

    const Status status = doc.render_page(0, target);
    ORACLE(is_documented_render_status(status),
           "render_page returned a status outside its documented set");
    ORACLE(status != Status::PdfNotOpen, "render_page said not-open on an open Document");
    ORACLE(status != Status::PdfRenderSizeInvalid,
           "render_page rejected a geometry required_buffer_bytes had accepted");
    ORACLE(status != Status::PdfBufferTooSmall,
           "render_page rejected a buffer sized by required_buffer_bytes itself");
    if (page_count > 0) {
        ORACLE(status != Status::PdfPageNotFound,
               "page 0 was not found in a document that reports pages");
    }

    // The row padding must survive. document.h defines `stride_bytes` as the
    // distance between row STARTS, so the bytes between width*4 and the
    // stride belong to the caller (on Android/iOS they are the platform
    // bitmap's own padding) and are not the renderer's to touch. This is the
    // same assertion tests/pdf_engine/pdf_reader_test.cpp makes for the
    // fixture; here it runs against every PDF the mutator invents. Only
    // checked on success: document.h says that on failure "the buffer's
    // contents are unspecified but every written byte stays inside
    // capacity_bytes", and ASan is what enforces the second half.
    if (uapp::is_ok(status) && geometry.extra_row_padding != 0) {
        const std::size_t row_bytes =
            static_cast<std::size_t>(geometry.width) * uapp::pdf::kRenderBytesPerPixel;
        for (int row = 0; row < geometry.height; ++row) {
            const std::size_t row_start = static_cast<std::size_t>(row) * stride;
            for (std::size_t i = row_bytes; i < stride; ++i) {
                ORACLE(pixels[row_start + i] == kPoison,
                       "the renderer wrote into the caller's row padding");
            }
        }
    }

    // The index boundary on the render path too. render_page checks it
    // independently of page_size (a different function with its own guard),
    // so it is worth its own assertion rather than being assumed to follow.
    const Status past_end = doc.render_page(page_count, target);
    ORACLE(past_end == Status::PdfPageNotFound,
           "render_page(page_count) was not PdfPageNotFound");
}

/// Everything that happens once a document is open.
void exercise_open_document(const Document& doc, std::size_t selector) {
    ORACLE(doc.is_open(), "open_buffer returned Ok without opening anything");

    // document.h: "Pages in the document, or 0 if none is open." A valid PDF
    // has at least one page, but a RECOVERED one legitimately may not — so 0
    // is allowed and only a negative count would be a contract break.
    const int page_count = doc.page_count();
    ORACLE(page_count >= 0, "page_count returned a negative number");

    measure_pages(doc, page_count);
    render_first_page(doc, page_count, selector);
}

}  // namespace

/// Pin PDFium up for the whole run.
///
/// Without this, every iteration would pay a full FPDF_InitLibrary /
/// FPDF_DestroyLibrary cycle — rebuilding the font cache and the codec
/// modules — which costs far more than parsing a small PDF and would make
/// this target measure PDFium's startup rather than its parser. The pin is a
/// function-local static, so it is constructed once and destroyed during
/// exit after every stack-scoped Document is long gone, which is exactly the
/// ordering rule document.h states ("every Document must be destroyed before
/// the last Library goes away").
extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    static const Library pin;
    (void)pin.holds_reference();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // One byte drives both derived inputs. Taken from the FRONT because a
    // PDF's first bytes are its header and the mutator keeps them stable
    // across a lineage, so a given corpus entry tends to keep exercising the
    // same geometry as it evolves — which makes a coverage gain attributable
    // to the bytes rather than to a reshuffled render target.
    const std::size_t selector = (size == 0) ? 0u : static_cast<std::size_t>(data[0]);

    Document     doc;
    const Status status = Document::open_buffer(data, size, doc);
    ORACLE(is_documented_open_status(status),
           "open_buffer returned a status outside its documented set");

    if (size == 0) {
        ORACLE(status == Status::FileEmpty, "an empty buffer did not report FileEmpty");
        ORACLE(!doc.is_open(), "an empty buffer produced an open Document");
        return 0;
    }

    if (uapp::is_ok(status)) {
        exercise_open_document(doc, selector);
        return 0;
    }

    // document.h: "`out` is overwritten on success and left closed on
    // failure; a failed open never leaves a half-open Document behind."
    ORACLE(!doc.is_open(), "a failed open left a half-open Document");

    // The encrypted branch. Retrying with a password is the only way this
    // target ever reaches PDFium's standard security handler — the
    // no-password call above stops at the /Encrypt dictionary — and it is
    // also the only place `open_buffer`'s three-argument overload, with its
    // embedded-NUL check and its std::string round trip, is exercised on
    // real input rather than on the one unit-test string.
    if (status == Status::PdfPasswordRequired) {
        const std::string_view password(kPasswords[selector % kPasswordCount]);

        Document     unlocked;
        const Status retry = Document::open_buffer(data, size, password, unlocked);
        ORACLE(is_documented_open_status(retry),
               "open_buffer with a password returned a status outside its documented set");
        ORACLE(retry != Status::InvalidArgument,
               "a password with no embedded NUL was rejected as invalid");

        if (uapp::is_ok(retry)) {
            exercise_open_document(unlocked, selector);
        } else {
            ORACLE(!unlocked.is_open(), "a failed password open left a half-open Document");
        }
    }

    return 0;
}
