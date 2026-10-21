// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/writer/include/uapp/pdf/writer.h — the PDF WRITING surface,
// new alongside pdf-engine/reader (viewing only, see document.h).
//
// WHY THIS EXISTS: `uapp::pdf::Document` can open and render a PDF but
// cannot create or modify one — pdf-engine's own header has said "editing,
// merge/split... NOT IMPLEMENTED" since it landed. Almost every "PDF
// toolkit" feature (merge, split, rotate, organize, images-to-PDF,
// scan-to-PDF) needs the ability to WRITE a PDF, not just read one, and
// nothing in this project could do that until this file.
//
// WHAT THIS IS, AND IS NOT: a minimal, image-based PDF writer. It builds a
// new PDF whose pages are each exactly one raster image (JPEG-encoded),
// packed with hand-written PDF syntax — header, image XObjects, page content
// streams, an xref table, trailer. It does NOT edit an existing PDF's
// object graph, does NOT preserve vector text/graphics, and does NOT touch
// PDFium at all (no dependency on it, deliberately — see CMakeLists.txt).
//
// WHY THIS IS STILL GENUINELY USEFUL, NOT A TOY: it is the real output format
// for a document scanner (CamScanner's whole product is "photo of a page ->
// PDF"), and composed with the EXISTING reader's render_page(), it gives
// honest (if lossy/rasterized) implementations of merge, split, rotate,
// reorder and page-extraction: render each source page to an image with the
// reader, optionally transform it, and repack with this writer. That
// composition is exactly what the plan at
// /Users/srikrishna/.claude/plans/squishy-roaming-comet.md's PDF-toolkit
// milestones build on top of this header.
//
// A real incremental-edit writer (preserving vector content, true in-place
// merge/split) is NOT IMPLEMENTED here and would need PDFium's own write-side
// API (FPDF_SaveAsCopy et al.), which core/capi and pdf-engine/reader do not
// expose today. That is future work, not a silent gap: this header's own
// tests prove round-trip correctness (write with this module, open and
// render with pdf-engine/reader, confirm pixels match), which is the honest
// bar a lossy rasterizing writer can actually meet.

#ifndef UAPP_PDF_ENGINE_WRITER_WRITER_H
#define UAPP_PDF_ENGINE_WRITER_WRITER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "uapp/status.h"

namespace uapp::pdf {

/// One page's source image. Always RGB (3 bytes/pixel, row-major,
/// top-to-bottom) — callers with RGBA input drop the alpha channel before
/// calling, since a printed/scanned page has no meaningful transparency.
struct PageImage final {
    const std::uint8_t* rgb_pixels = nullptr;  ///< width * height * 3 bytes, borrowed
    std::int32_t         width      = 0;
    std::int32_t         height     = 0;

    /// Clockwise rotation applied to this page, in quarter turns: 0, 1, 2 or
    /// 3 (i.e. 0, 90, 180, 270 degrees). Rotation is stored as a PDF page
    /// `/Rotate` entry rather than by re-encoding the image, so it is exact
    /// and free — this is what a real "rotate PDF" tool built on this writer
    /// uses instead of resampling pixels.
    std::int32_t rotate_quarter_turns = 0;
};

struct WriteOptions final {
    /// JPEG quality, 1-100. 90 is a reasonable default for a scanned
    /// document: visually lossless at normal reading zoom, far smaller than
    /// PNG/raw for a photographed page.
    std::int32_t jpeg_quality = 90;

    /// Pixels-per-inch the source images were captured/rendered at, used to
    /// convert pixel dimensions into PDF's physical unit (1/72 inch,
    /// "points"). 150 matches this project's own default PDF render DPI
    /// (api/src/routes_pdf.cpp's render calls), so a round trip through
    /// render -> write -> reopen reproduces the original page size.
    double source_dpi = 150.0;
};

/// Limits this module enforces — read at runtime for the same reason every
/// other limits struct in this project is (a caller must not hard-code a
/// bound this build does not actually enforce).
struct WriterLimits final {
    std::int32_t max_pages          = 2000;    ///< a single PDF this module will produce
    std::int32_t max_dimension      = 20000;   ///< longest side of any one page image, pixels
    std::size_t  max_total_pixels   = 200u * 1024u * 1024u;  ///< sum over every page
};

[[nodiscard]] WriterLimits writer_limits() noexcept;

/// Build a new PDF with one page per entry in `pages`, in order.
///
/// Errors: InvalidArgument (empty `pages`, a null/zero-sized image, a
/// rotate value outside [0,3]), ConversionOutputTooLarge (page count or a
/// single dimension past WriterLimits, or the sum of all pages' pixel
/// counts past max_total_pixels — the limit this module's own encode
/// buffer is sized against), Internal (the JPEG encoder refused an input,
/// which should not happen for any image that passed the checks above).
///
/// On any error `out` is left empty — never partially filled.
[[nodiscard]] Status write_images_as_pdf(const std::vector<PageImage>& pages,
                                         const WriteOptions& options,
                                         std::vector<std::uint8_t>& out);

/// Decodes an arbitrary compressed image (JPEG, PNG, BMP, GIF, ... — whatever
/// stb_image.h recognises from the byte signature, format is never taken on
/// faith from a filename) into top-to-bottom RGB pixels, dropping any alpha
/// channel. This is the de-compression half the upload-time "images to PDF"
/// and document-scanner pipelines need before they can hand pixels to
/// write_images_as_pdf() above or to an enhancement/crop step.
///
/// `out_pixels` is sized to exactly width*height*3 bytes on success and left
/// empty on failure. Errors: InvalidArgument (null/zero-sized input),
/// ConversionInputMalformed (bytes are not a recognisable image, or decode
/// to a size past WriterLimits), Internal (decoder refused for another
/// reason).
[[nodiscard]] Status decode_image_to_rgb(const std::uint8_t* bytes, std::size_t len,
                                         std::vector<std::uint8_t>& out_pixels,
                                         std::int32_t& out_width, std::int32_t& out_height);

}  // namespace uapp::pdf

#endif  // UAPP_PDF_ENGINE_WRITER_WRITER_H
