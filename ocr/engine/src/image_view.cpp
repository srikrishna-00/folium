// SPDX-License-Identifier: Apache-2.0
//
// ocr/engine/image_view.cpp — the arithmetic that decides whether a
// caller-supplied source image is safe to read from.
//
// This file contains NO Tesseract code, deliberately, and the reason is the
// one pdf-engine/reader/src/render_target.cpp gives for the same split: this
// is the whole of the bounds reasoning for an untrusted-geometry entry point,
// so it is worth being able to read, review and test without an OCR library
// anywhere near it. Every multiplication below can overflow on a hostile
// input, and every one of them is checked BEFORE it is performed rather than
// after — `a * b / b == a` after the fact is undefined once a signed
// multiplication has already overflowed and merely fragile on an unsigned one.
//
// It is a near-copy of render_target.cpp's structure, which is intentional
// (uapp/ocr/engine.h design rule 1: the buffer conventions must match so a
// pdf-render -> OCR pipeline needs no conversion step) and NOT a candidate
// for being factored into a shared helper. Two reasons it stays duplicated:
//
//   1. `bytes_per_pixel` is a variable here and the constant 4 there, and the
//      4-byte stride ALIGNMENT rule PDFium needs does not apply to Tesseract.
//      A shared helper would have to be parameterised on both, at which point
//      it is a different function that happens to resemble two others.
//   2. A shared helper would have to live in core/common, which would make
//      core/ know about raster buffers. architecture.md section 4 rule 3
//      points the dependency the other way.

#include "uapp/ocr/engine.h"

#include <cstddef>
#include <limits>

namespace uapp::ocr {
namespace {

/// True iff `a * b` fits in a std::size_t. Division, not multiplication, so
/// the overflow never happens in the first place.
bool mul_fits(std::size_t a, std::size_t b) noexcept {
    if (a == 0 || b == 0) { return true; }
    return a <= std::numeric_limits<std::size_t>::max() / b;
}

bool dimension_ok(int value) noexcept { return value >= 1 && value <= kMaxImageDimension; }

std::size_t bytes_per_pixel_for(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::Rgba8888: return 4;
        case PixelFormat::Gray8:    return 1;
    }
    // Reached only if a caller casts a bogus integer into PixelFormat. 0 is
    // the safe answer: every caller below treats it as "unusable geometry",
    // so an invented format is refused rather than guessed at.
    return 0;
}

}  // namespace

std::size_t ImageView::bytes_per_pixel() const noexcept { return bytes_per_pixel_for(format); }

std::size_t required_image_bytes(int width, int height, std::size_t stride_bytes,
                                 PixelFormat format) noexcept {
    if (!dimension_ok(width) || !dimension_ok(height)) { return 0; }

    const std::size_t bpp = bytes_per_pixel_for(format);
    if (bpp == 0) { return 0; }

    // Both dimensions are now known to be in [1, 20000], so this product is
    // at most 4e8 and cannot overflow a std::size_t on any supported
    // platform. The area cap is checked on the product rather than on the
    // dimensions because 20000 x 20000 passes both dimension checks and is
    // 400 Mpx — see kMaxImagePixels for why that matters on a 2 GB device.
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixels > kMaxImagePixels) { return 0; }

    // width <= 20000 and bpp <= 4, so this is at most 80000.
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bpp;

    const std::size_t stride = (stride_bytes == 0) ? row_bytes : stride_bytes;
    if (stride < row_bytes) {
        // Rows would overlap. Tesseract reads with the stride it is given, so
        // this would make it read pixels the caller never meant to supply —
        // and, with a large height and a tiny stride, read well past the
        // buffer while every per-row offset still looked plausible.
        return 0;
    }

    // Deliberately NO `stride % 4` check here, unlike pdf::required_buffer_
    // bytes(). That rule exists because PDFium's 32 bpp bitmaps abort the
    // PROCESS on an unaligned scanline; Tesseract's SetImage takes
    // bytes_per_line as an arbitrary count and has no such requirement.
    // Copying the constraint over would reject valid Gray8 buffers for a
    // reason that does not apply to this library.

    const std::size_t rows = static_cast<std::size_t>(height);
    if (!mul_fits(stride, rows)) { return 0; }

    // stride * height, not stride * (height - 1) + row_bytes. Same choice
    // render_target.cpp makes and for the same reason: the tighter bound is
    // what is actually read, but requiring the full rectangle costs at most
    // one row's padding and makes the safe size the obvious one instead of
    // something every later edit has to re-derive.
    return stride * rows;
}

std::size_t ImageView::effective_stride() const noexcept {
    if (!dimension_ok(width)) { return 0; }
    const std::size_t bpp = bytes_per_pixel_for(format);
    if (bpp == 0) { return 0; }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bpp;
    return (stride_bytes == 0) ? row_bytes : stride_bytes;
}

Status ImageView::validate() const noexcept {
    if (pixels == nullptr) { return Status::InvalidArgument; }

    const std::size_t needed = required_image_bytes(width, height, stride_bytes, format);
    if (needed == 0) {
        // required_image_bytes() folds every "this geometry is not usable"
        // case into 0: non-positive, over a dimension cap, over the pixel
        // cap, an unrecognised format, a stride below the row width, or an
        // overflowing product. They are one class of caller error with one
        // fix — stop asking for that — and splitting them would produce
        // codes a caller cannot act on differently. Same argument
        // pdf::RenderTarget::validate() makes for PdfRenderSizeInvalid, and
        // the same argument uapp/status.h makes for FileEmpty.
        return Status::OcrImageSizeInvalid;
    }
    if (capacity_bytes < needed) { return Status::OcrImageBufferTooSmall; }
    return Status::Ok;
}

}  // namespace uapp::ocr
