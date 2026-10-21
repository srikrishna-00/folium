// SPDX-License-Identifier: Apache-2.0
//
// tests/pdf_engine/pdf_reader_test.cpp — pdf-engine/reader against real PDFium.
//
// Case names are prefixed so ctest can split them into three entries whose
// failures mean different things (tests/CMakeLists.txt):
//
//   pdf.read.    — the feature works on a well-formed document
//   pdf.render.  — pixels land in the caller's buffer, correctly and only
//                  inside it
//   pdf.malformed — hostile or broken input produces a defined Status and
//                  does not crash
//
// The third group is the one that matters most. Spec section 29 lists the
// PDF parser first among the fuzz targets and docs/risk-register.md treats it
// as the highest-risk parser in the product; every one of those cases is a
// buffer PDFium will genuinely try to parse.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_framework.h"
#include "uapp/pdf/document.h"
#include "uapp/pdf/writer.h"
#include "uapp/status.h"

#include "pdf_fixture.h"

namespace {

namespace fx = uapp::test::pdf;

using uapp::Status;
using uapp::pdf::Document;
using uapp::pdf::Library;
using uapp::pdf::PageSize;
using uapp::pdf::RenderTarget;

constexpr double kEpsilonPoints = 0.01;

bool near_enough(double a, double b) noexcept {
    const double diff = a - b;
    return (diff < 0 ? -diff : diff) < kEpsilonPoints;
}

/// A caller-owned pixel buffer, pre-poisoned.
///
/// 0x00 everywhere is not a neutral choice — it is the one value that makes
/// "the renderer wrote nothing" indistinguishable from "the renderer wrote
/// transparent black". So the buffer is filled with a recognisable poison
/// byte instead, and the assertions can tell "untouched" from "written".
struct PixelBuffer final {
    static constexpr std::uint8_t kPoison = 0x5A;

    std::vector<std::uint8_t> bytes;
    int                       width  = 0;
    int                       height = 0;
    std::size_t               stride = 0;

    PixelBuffer(int w, int h, std::size_t extra_row_padding = 0)
        : width(w), height(h) {
        stride = static_cast<std::size_t>(w) * 4 + extra_row_padding;
        bytes.assign(stride * static_cast<std::size_t>(h), kPoison);
    }

    [[nodiscard]] RenderTarget target() noexcept {
        RenderTarget t;
        t.pixels         = bytes.data();
        t.capacity_bytes = bytes.size();
        t.width          = width;
        t.height         = height;
        t.stride_bytes   = stride;
        return t;
    }

    [[nodiscard]] const std::uint8_t* pixel_at(int x, int y) const noexcept {
        return bytes.data() + static_cast<std::size_t>(y) * stride +
               static_cast<std::size_t>(x) * 4;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// pdf.read — the happy path
// ---------------------------------------------------------------------------

UAPP_TEST("pdf.read opens a valid two-page PDF from a buffer") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    UAPP_REQUIRE(!pdf.empty());

    Document doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));
    UAPP_CHECK(doc.is_open());
    UAPP_CHECK(doc.page_count() == fx::kFixturePageCount);
}

UAPP_TEST("pdf.read reports each page's own MediaBox in points") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    PageSize page0;
    UAPP_REQUIRE_OK(doc.page_size(0, page0));
    UAPP_CHECK_MSG(near_enough(page0.width_points, fx::kPage0WidthPoints),
                   "page 0 width");
    UAPP_CHECK_MSG(near_enough(page0.height_points, fx::kPage0HeightPoints),
                   "page 0 height");

    // A second, differently sized page: proves the size comes from the page
    // rather than from the document or from a default.
    PageSize page1;
    UAPP_REQUIRE_OK(doc.page_size(1, page1));
    UAPP_CHECK_MSG(near_enough(page1.width_points, fx::kPage1WidthPoints),
                   "page 1 width");
    UAPP_CHECK_MSG(near_enough(page1.height_points, fx::kPage1HeightPoints),
                   "page 1 height");
}

UAPP_TEST("pdf.read a moved-from Document is empty and the moved-to one works") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        source;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), source));

    Document moved(std::move(source));
    UAPP_CHECK(moved.is_open());
    UAPP_CHECK(moved.page_count() == fx::kFixturePageCount);

    // NOLINTNEXTLINE(bugprone-use-after-move) — asserting the moved-from
    // state is defined is the point of the case.
    UAPP_CHECK(!source.is_open());
    UAPP_CHECK(source.page_count() == 0);
}

UAPP_TEST("pdf.read close is idempotent and leaves a usable object") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    doc.close();
    doc.close();  // twice: a double close must not be a double free
    UAPP_CHECK(!doc.is_open());

    // ...and the same object can be reopened.
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));
    UAPP_CHECK(doc.page_count() == fx::kFixturePageCount);
}

UAPP_TEST("pdf.read PDFium's global refcount returns to zero") {
    // The lifecycle claim in document.h is only worth making if it is
    // checked: if this drifts, the symptom in production is either PDFium
    // torn down under a live document (a crash) or never torn down at all
    // (a leak), and neither shows up in any other case here.
    const int before = Library::reference_count();

    {
        const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
        Document                        a;
        Document                        b;
        UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), a));
        UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), b));
        UAPP_CHECK(Library::reference_count() >= before + 2);
    }

    UAPP_CHECK_MSG(Library::reference_count() == before,
                   "PDFium reference count did not return to its starting value");
}

UAPP_TEST("pdf.read an explicit Library reference keeps PDFium up across opens") {
    const Library pin;
    UAPP_CHECK(pin.holds_reference());
    const int with_pin = Library::reference_count();
    UAPP_CHECK(with_pin >= 1);

    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    for (int i = 0; i < 3; ++i) {
        Document doc;
        UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));
        UAPP_CHECK(doc.page_count() == fx::kFixturePageCount);
    }
    UAPP_CHECK(Library::reference_count() == with_pin);
}

// ---------------------------------------------------------------------------
// pdf.render — pixels, into the caller's buffer
// ---------------------------------------------------------------------------

UAPP_TEST("pdf.render writes real pixels into the caller's buffer") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    // 200x100 points at 1 px/pt, so PDF user space and device space line up
    // and the expected rectangle can be computed by hand.
    PixelBuffer buffer(200, 100);
    UAPP_REQUIRE_OK(doc.render_page(0, buffer.target()));

    // 1. Something was written. Poison bytes surviving would mean the render
    //    silently did nothing — the failure mode a "buffer is not all zero"
    //    check misses when the allocator happens to hand back zeroed pages.
    std::size_t poison_left = 0;
    for (std::uint8_t byte : buffer.bytes) {
        if (byte == PixelBuffer::kPoison) { ++poison_left; }
    }
    UAPP_CHECK_MSG(poison_left == 0, "some bytes were never written by the renderer");

    // 2. The buffer is not uniformly one value, i.e. the page was actually
    //    rasterised rather than merely cleared to white.
    bool                any_difference = false;
    const std::uint8_t* first          = buffer.bytes.data();
    for (std::size_t i = 4; i < buffer.bytes.size(); i += 4) {
        if (buffer.bytes[i] != first[0]) {
            any_difference = true;
            break;
        }
    }
    UAPP_CHECK_MSG(any_difference, "every pixel is identical — nothing was drawn");

    // 3. The marks are where the fixture put them. PDF user space has its
    //    origin at the BOTTOM-left and device space at the top-left, so the
    //    black band at y=[20,80] in points appears at rows [20,80] measured
    //    from the top in this symmetric case; a point inside the rectangle
    //    and a point outside it are sampled separately so a flipped render
    //    could not pass by accident on the inside sample alone.
    const std::uint8_t* inside = buffer.pixel_at(100, 50);
    UAPP_CHECK_MSG(inside[0] < 64 && inside[1] < 64 && inside[2] < 64,
                   "centre of the page should be inside the black rectangle");
    UAPP_CHECK_MSG(inside[3] == 0xFF, "alpha should be opaque");

    const std::uint8_t* outside = buffer.pixel_at(2, 2);
    UAPP_CHECK_MSG(outside[0] > 200 && outside[1] > 200 && outside[2] > 200,
                   "corner of the page should be the white background");
    UAPP_CHECK_MSG(outside[3] == 0xFF, "background alpha should be opaque");
}

// ---------------------------------------------------------------------------
// pdf.roundtrip — pdf-engine/writer's output, opened by THIS module
// ---------------------------------------------------------------------------
//
// The one test in the whole suite that proves both halves of round-trip
// correctness in a single process: pdf-engine/writer builds a PDF from
// scratch with no PDFium involvement at all, and THIS file — which already
// has a real PDFium underneath it — opens and renders that exact output.
// pdf_writer_test.cpp (unconditional, no PDFium needed) checks the writer's
// syntax is well-formed and that stb's own decoder can read back what stb's
// own encoder wrote; this is the stronger claim neither side can check
// alone: a REAL PDF reader, independent of the code that wrote the file,
// opens it successfully and reports the pixels that were actually asked for.

UAPP_TEST("pdf.roundtrip a PDF written by pdf-engine/writer opens and renders correctly") {
    using uapp::pdf::PageImage;
    using uapp::pdf::WriteOptions;
    using uapp::pdf::write_images_as_pdf;

    // A 120x80 image: a solid dark rectangle in the left half, light in the
    // right half — two widely-separated sample points make a flipped or
    // garbled render impossible to mistake for a pass.
    const int                 width  = 120;
    const int                 height = 80;
    const auto                w      = static_cast<std::size_t>(width);
    const auto                h      = static_cast<std::size_t>(height);
    std::vector<std::uint8_t> pixels(w * h * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t  idx   = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 3;
            const std::uint8_t value = (x < width / 2) ? 10 : 240;
            pixels[idx + 0] = value;
            pixels[idx + 1] = value;
            pixels[idx + 2] = value;
        }
    }

    PageImage page;
    page.rgb_pixels = pixels.data();
    page.width      = width;
    page.height     = height;

    std::vector<std::uint8_t> pdf_bytes;
    UAPP_REQUIRE_OK(write_images_as_pdf({page}, WriteOptions{}, pdf_bytes));

    Document doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf_bytes.data(), pdf_bytes.size(), doc));
    UAPP_CHECK_MSG(doc.page_count() == 1, "got " + std::to_string(doc.page_count()));

    // Render at the SOURCE image's own pixel dimensions, so comparison is
    // 1:1 regardless of the writer's points-per-pixel (source_dpi) choice —
    // render_page fills exactly the requested width/height no matter what
    // the PDF's own MediaBox says.
    PixelBuffer buffer(width, height);
    UAPP_REQUIRE_OK(doc.render_page(0, buffer.target()));

    // JPEG is lossy, so allow real tolerance rather than an exact match —
    // the point is "this is recognisably the image we wrote", not bit-exact
    // reproduction.
    const std::uint8_t* dark_region  = buffer.pixel_at(width / 4, height / 2);
    const std::uint8_t* light_region = buffer.pixel_at(3 * width / 4, height / 2);
    UAPP_CHECK_MSG(dark_region[0] < 60, "left half should still read as dark; got " +
                                            std::to_string(static_cast<int>(dark_region[0])));
    UAPP_CHECK_MSG(light_region[0] > 190, "right half should still read as light; got " +
                                              std::to_string(static_cast<int>(light_region[0])));
    UAPP_CHECK(dark_region[3] == 0xFF);
    UAPP_CHECK(light_region[3] == 0xFF);
}

UAPP_TEST("pdf.render honours a padded stride and stays inside the buffer") {
    // The Android/iOS case from SP-5 F-2: a platform bitmap's stride is
    // frequently wider than width*4. If the renderer ignored the stride, the
    // guard bytes at the end of each row would be overwritten and the image
    // would shear.
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    // 36, not 32: a padding that is not itself a power of two, so a renderer
    // that quietly rounded the stride up or down would produce a sheared
    // image rather than an accidentally-correct one. It is still a multiple
    // of 4, which PDFium requires (see RenderTarget::stride_bytes) — the
    // unaligned case is asserted separately, as an error, below.
    constexpr std::size_t kPadding = 36;
    PixelBuffer           buffer(64, 32, kPadding);
    UAPP_REQUIRE_OK(doc.render_page(0, buffer.target()));

    std::size_t padding_touched = 0;
    for (int row = 0; row < buffer.height; ++row) {
        const std::size_t row_start = static_cast<std::size_t>(row) * buffer.stride;
        for (std::size_t i = static_cast<std::size_t>(buffer.width) * 4; i < buffer.stride; ++i) {
            if (buffer.bytes[row_start + i] != PixelBuffer::kPoison) { ++padding_touched; }
        }
    }
    UAPP_CHECK_MSG(padding_touched == 0, "the renderer wrote into the row padding");

    // And the pixel area itself was still written.
    const std::uint8_t* centre = buffer.pixel_at(32, 16);
    UAPP_CHECK(centre[0] != PixelBuffer::kPoison || centre[1] != PixelBuffer::kPoison);
}

UAPP_TEST("pdf.render a second call over the same buffer is repeatable") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    PixelBuffer first(80, 40);
    PixelBuffer second(80, 40);
    UAPP_REQUIRE_OK(doc.render_page(0, first.target()));
    UAPP_REQUIRE_OK(doc.render_page(0, second.target()));
    UAPP_CHECK_MSG(first.bytes == second.bytes,
                   "rendering the same page twice produced different pixels");
}

UAPP_TEST("pdf.render the blank second page is all background, not garbage") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    PixelBuffer buffer(30, 40);
    UAPP_REQUIRE_OK(doc.render_page(1, buffer.target()));

    bool all_white = true;
    for (std::size_t i = 0; i < buffer.bytes.size(); i += 4) {
        if (buffer.bytes[i] != 0xFF || buffer.bytes[i + 1] != 0xFF ||
            buffer.bytes[i + 2] != 0xFF || buffer.bytes[i + 3] != 0xFF) {
            all_white = false;
            break;
        }
    }
    UAPP_CHECK_MSG(all_white, "a blank page must clear to opaque white, not to poison");
}

// ---------------------------------------------------------------------------
// pdf.malformed — broken and hostile input
// ---------------------------------------------------------------------------

UAPP_TEST("pdf.malformed an empty buffer is FILE_EMPTY, not a crash") {
    Document doc;
    UAPP_REQUIRE_STATUS(Document::open_buffer(nullptr, 0, doc), Status::FileEmpty);
    UAPP_CHECK(!doc.is_open());

    const std::vector<std::uint8_t> empty;
    UAPP_REQUIRE_STATUS(Document::open_buffer(empty.data(), empty.size(), doc),
                        Status::FileEmpty);
    UAPP_CHECK(!doc.is_open());
}

UAPP_TEST("pdf.malformed a null pointer with a non-zero size is rejected") {
    Document doc;
    UAPP_REQUIRE_STATUS(Document::open_buffer(nullptr, 4096, doc), Status::InvalidArgument);
    UAPP_CHECK(!doc.is_open());
}

UAPP_TEST("pdf.malformed truncated PDF bytes give a defined error") {
    const std::vector<std::uint8_t> full = fx::two_page_pdf();
    UAPP_REQUIRE(full.size() > 100);

    // A spread of truncation points, because the parser takes different
    // paths depending on how much of the file survived: mid-header,
    // mid-object, and "everything but the xref table and trailer".
    const std::size_t cuts[] = {4, 9, 40, 120, full.size() - 40, full.size() - 1};
    for (std::size_t keep : cuts) {
        const std::vector<std::uint8_t> cut = fx::truncated_pdf(keep);
        Document                        doc;
        const Status                    status =
            Document::open_buffer(cut.data(), cut.size(), doc);

        if (uapp::is_ok(status)) {
            // PDFium rebuilds the cross-reference table when the trailer is
            // unusable, so a late cut CAN legitimately open. That is a
            // recovery, not a failure — but the recovered document must
            // still behave, so it is held to the same contract rather than
            // waved through.
            UAPP_CHECK_MSG(doc.is_open(), "open returned Ok without opening anything");
            PixelBuffer buffer(16, 16);
            const Status render = doc.render_page(0, buffer.target());
            UAPP_CHECK_MSG(uapp::is_ok(render) || render == Status::PdfPageNotFound ||
                               render == Status::PdfPageLoadFailed,
                           "a recovered document rendered with an undefined status");
        } else {
            UAPP_CHECK_MSG(status == Status::PdfMalformed ||
                               status == Status::PdfPasswordRequired ||
                               status == Status::PdfSecurityUnsupported,
                           "truncation produced a status outside the documented set");
            UAPP_CHECK(!doc.is_open());
        }
    }
}

UAPP_TEST("pdf.malformed bytes that are not a PDF are PDF_MALFORMED") {
    const std::vector<std::uint8_t> text = fx::not_a_pdf();
    Document                        doc;
    UAPP_REQUIRE_STATUS(Document::open_buffer(text.data(), text.size(), doc),
                        Status::PdfMalformed);
    UAPP_CHECK(!doc.is_open());
}

UAPP_TEST("pdf.malformed a bare %PDF- header with no body is rejected") {
    // The case where a magic-number sniffer and a parser must be allowed to
    // disagree: file-engine/detection would call this a PDF.
    const std::vector<std::uint8_t> header = fx::header_only_pdf();
    Document                        doc;
    UAPP_REQUIRE_STATUS(Document::open_buffer(header.data(), header.size(), doc),
                        Status::PdfMalformed);
    UAPP_CHECK(!doc.is_open());
}

UAPP_TEST("pdf.malformed a startxref pointing past EOF does not read out of bounds") {
    const std::vector<std::uint8_t> pdf = fx::bad_startxref_pdf();
    Document                        doc;
    const Status status = Document::open_buffer(pdf.data(), pdf.size(), doc);

    // PDFium is expected to rebuild the xref and recover here; what is being
    // asserted is that it terminates with a defined answer and, under ASan,
    // without reading past the buffer. Either outcome is acceptable; an
    // undefined third one is not.
    UAPP_CHECK_MSG(uapp::is_ok(status) || status == Status::PdfMalformed,
                   "a corrupt startxref produced an undocumented status");
    if (uapp::is_ok(status)) {
        PixelBuffer  buffer(20, 20);
        const Status render = doc.render_page(0, buffer.target());
        UAPP_CHECK(uapp::is_ok(render) || render == Status::PdfPageNotFound ||
                   render == Status::PdfPageLoadFailed);
    }
}

UAPP_TEST("pdf.malformed an in-range page that will not load is PDF_PAGE_LOAD_FAILED") {
    // REGRESSION — found by tests/fuzz/fuzz_pdf_reader at 139 executions,
    // mutating two_page_pdf()'s second Kid from "4 0 R" to "44 0 R".
    //
    // The page tree says /Count 2, so page_count() is 2 and index 1 is in
    // range; object 44 does not exist, so the page cannot be resolved. Both
    // page-level entry points used to answer that by reading PDFium's
    // process-global FPDF_GetLastError(), which neither
    // FPDF_GetPageSizeByIndexF nor FPDF_LoadPage writes — so it still held
    // FPDF_ERR_SUCCESS from the document load that had just succeeded, and
    // the mapper turned "no error" into Status::Internal.
    //
    // Two things were wrong with that and this case pins both. Internal is
    // not in either function's documented return set, so a caller switching
    // on the documented statuses fell through to its default branch; and
    // status.h defines Internal as "a bug in us, not in the caller's input"
    // for a condition caused entirely by the caller's input, which means
    // attacker-supplied bytes could make the product report an internal
    // defect.
    const std::vector<std::uint8_t> pdf = fx::dangling_page_reference_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));
    UAPP_CHECK_MSG(doc.page_count() == 2, "the /Count is what PDFium reports, broken or not");

    // Page 0 is intact and must be unaffected by the fix.
    PageSize page0;
    UAPP_REQUIRE_OK(doc.page_size(0, page0));
    UAPP_CHECK(near_enough(page0.width_points, fx::kPage0WidthPoints));

    // Page 1 is the dangling one: in range, unresolvable.
    PageSize page1;
    UAPP_REQUIRE_STATUS(doc.page_size(1, page1), Status::PdfPageLoadFailed);

    PixelBuffer buffer(16, 16);
    UAPP_REQUIRE_STATUS(doc.render_page(1, buffer.target()), Status::PdfPageLoadFailed);

    // And the index boundary still means what it meant: "out of range" and
    // "in range but broken" are different answers, not the same one.
    UAPP_REQUIRE_STATUS(doc.page_size(2, page1), Status::PdfPageNotFound);
    UAPP_REQUIRE_STATUS(doc.render_page(2, buffer.target()), Status::PdfPageNotFound);
}

UAPP_TEST("pdf.malformed operations on an unopened Document are PDF_NOT_OPEN") {
    Document doc;
    PageSize size;
    UAPP_REQUIRE_STATUS(doc.page_size(0, size), Status::PdfNotOpen);

    PixelBuffer buffer(8, 8);
    UAPP_REQUIRE_STATUS(doc.render_page(0, buffer.target()), Status::PdfNotOpen);
    UAPP_CHECK(doc.page_count() == 0);
}

UAPP_TEST("pdf.malformed out-of-range page indices are PDF_PAGE_NOT_FOUND") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    PageSize size;
    UAPP_REQUIRE_STATUS(doc.page_size(-1, size), Status::PdfPageNotFound);
    UAPP_REQUIRE_STATUS(doc.page_size(fx::kFixturePageCount, size), Status::PdfPageNotFound);
    UAPP_REQUIRE_STATUS(doc.page_size(1 << 20, size), Status::PdfPageNotFound);

    PixelBuffer buffer(8, 8);
    UAPP_REQUIRE_STATUS(doc.render_page(-1, buffer.target()), Status::PdfPageNotFound);
    UAPP_REQUIRE_STATUS(doc.render_page(fx::kFixturePageCount, buffer.target()),
                        Status::PdfPageNotFound);
}

UAPP_TEST("pdf.malformed a bad render target is refused before anything is parsed") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    UAPP_REQUIRE_OK(Document::open_buffer(pdf.data(), pdf.size(), doc));

    std::vector<std::uint8_t> pixels(64 * 64 * 4, 0);

    RenderTarget null_pixels;
    null_pixels.pixels         = nullptr;
    null_pixels.capacity_bytes = pixels.size();
    null_pixels.width          = 8;
    null_pixels.height         = 8;
    UAPP_REQUIRE_STATUS(doc.render_page(0, null_pixels), Status::InvalidArgument);

    RenderTarget zero_width;
    zero_width.pixels         = pixels.data();
    zero_width.capacity_bytes = pixels.size();
    zero_width.width          = 0;
    zero_width.height         = 8;
    UAPP_REQUIRE_STATUS(doc.render_page(0, zero_width), Status::PdfRenderSizeInvalid);

    RenderTarget negative_height;
    negative_height.pixels         = pixels.data();
    negative_height.capacity_bytes = pixels.size();
    negative_height.width          = 8;
    negative_height.height         = -1;
    UAPP_REQUIRE_STATUS(doc.render_page(0, negative_height), Status::PdfRenderSizeInvalid);

    RenderTarget enormous;
    enormous.pixels         = pixels.data();
    enormous.capacity_bytes = pixels.size();
    enormous.width          = uapp::pdf::kMaxRenderDimension + 1;
    enormous.height         = 8;
    UAPP_REQUIRE_STATUS(doc.render_page(0, enormous), Status::PdfRenderSizeInvalid);

    RenderTarget short_stride;
    short_stride.pixels         = pixels.data();
    short_stride.capacity_bytes = pixels.size();
    short_stride.width          = 8;
    short_stride.height         = 8;
    short_stride.stride_bytes   = 8;  // less than width * 4: rows would overlap
    UAPP_REQUIRE_STATUS(doc.render_page(0, short_stride), Status::PdfRenderSizeInvalid);

    // A stride that is wide enough but not 4-byte aligned. This one is not
    // hypothetical: passing it through to PDFium aborts the whole process on
    // an internal CHECK (SIGTRAP, no message), which is why the check lives
    // in RenderTarget::validate() and why there is a case for it.
    RenderTarget unaligned_stride;
    unaligned_stride.pixels         = pixels.data();
    unaligned_stride.capacity_bytes = pixels.size();
    unaligned_stride.width          = 8;
    unaligned_stride.height         = 8;
    unaligned_stride.stride_bytes   = 8 * 4 + 1;
    UAPP_REQUIRE_STATUS(doc.render_page(0, unaligned_stride), Status::PdfRenderSizeInvalid);

    RenderTarget too_small;
    too_small.pixels         = pixels.data();
    too_small.capacity_bytes = 16;  // truthfully tiny
    too_small.width          = 64;
    too_small.height         = 64;
    UAPP_REQUIRE_STATUS(doc.render_page(0, too_small), Status::PdfBufferTooSmall);
}

UAPP_TEST("pdf.malformed buffer-size arithmetic cannot be made to overflow") {
    // required_buffer_bytes() is the only place a caller-controlled
    // multiplication happens on the render path, so it is checked directly
    // rather than only through render_page.
    using uapp::pdf::kMaxRenderDimension;
    using uapp::pdf::required_buffer_bytes;

    UAPP_CHECK(required_buffer_bytes(10, 10, 0) == 400);
    UAPP_CHECK(required_buffer_bytes(10, 10, 64) == 640);

    // 0 is the single "unusable" answer, never a legitimate size.
    UAPP_CHECK(required_buffer_bytes(0, 10, 0) == 0);
    UAPP_CHECK(required_buffer_bytes(10, 0, 0) == 0);
    UAPP_CHECK(required_buffer_bytes(-1, 10, 0) == 0);
    UAPP_CHECK(required_buffer_bytes(10, -1, 0) == 0);
    UAPP_CHECK(required_buffer_bytes(kMaxRenderDimension + 1, 10, 0) == 0);
    UAPP_CHECK(required_buffer_bytes(10, kMaxRenderDimension + 1, 0) == 0);
    UAPP_CHECK_MSG(required_buffer_bytes(10, 10, 39) == 0, "stride below width*4 must be refused");
    UAPP_CHECK_MSG(required_buffer_bytes(10, 10, 41) == 0,
                   "stride that is not a multiple of 4 must be refused");
    UAPP_CHECK_MSG(required_buffer_bytes(10, 10, 44) == 440,
                   "a wider, 4-byte-aligned stride must be accepted");

    // A stride that would overflow size_t when multiplied by the height.
    const std::size_t huge = static_cast<std::size_t>(-1) / 2;
    UAPP_CHECK(required_buffer_bytes(10, 100, huge) == 0);

    // The largest legitimate request must still be computable.
    UAPP_CHECK(required_buffer_bytes(kMaxRenderDimension, kMaxRenderDimension, 0) ==
               static_cast<std::size_t>(kMaxRenderDimension) *
                   static_cast<std::size_t>(kMaxRenderDimension) * 4);
}

UAPP_TEST("pdf.malformed a password containing NUL is refused, not truncated") {
    const std::vector<std::uint8_t> pdf = fx::two_page_pdf();
    Document                        doc;
    const std::string_view          nasty("abc\0def", 7);
    UAPP_REQUIRE_STATUS(Document::open_buffer(pdf.data(), pdf.size(), nasty, doc),
                        Status::InvalidArgument);
    UAPP_CHECK(!doc.is_open());
}

UAPP_TEST("pdf.malformed every byte position truncation terminates without crashing") {
    // A cheap stand-in for the fuzz target that section 29 requires and this
    // slice does not ship: walk the truncation point across the whole
    // fixture. It is exhaustive over one mutation rather than random over
    // all of them, so it is not fuzzing — but it does mean every prefix of a
    // real PDF has been fed to the parser at least once.
    const std::vector<std::uint8_t> full = fx::two_page_pdf();
    int                             undefined = 0;

    for (std::size_t keep = 1; keep < full.size(); ++keep) {
        Document     doc;
        const Status status = Document::open_buffer(full.data(), keep, doc);
        const bool   defined = uapp::is_ok(status) || status == Status::PdfMalformed ||
                             status == Status::PdfPasswordRequired ||
                             status == Status::PdfSecurityUnsupported;
        if (!defined) { ++undefined; }
    }

    UAPP_CHECK_MSG(undefined == 0, "some prefix produced a status outside the documented set");
}
