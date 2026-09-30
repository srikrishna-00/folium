// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/reader/render_target.cpp — the arithmetic that decides whether a
// caller-supplied pixel buffer is safe to write into.
//
// This file contains NO PDFium code, deliberately. It is the whole of the
// bounds reasoning for the zero-copy render path (SP-5 F-2: PDFium writes
// straight into the caller's memory), so it is worth being able to read,
// review and test it without a PDF anywhere near it. Every multiplication
// below can overflow on a hostile input, and every one of them is checked
// before it is performed rather than after — `a * b / b == a` after the fact
// is undefined once the multiplication has already overflowed a signed type
// and merely fragile on an unsigned one.

#include "uapp/pdf/document.h"

#include <cstddef>
#include <limits>

namespace uapp::pdf {
namespace {

/// True iff `a * b` fits in a std::size_t. Division, not multiplication, so
/// the overflow never happens in the first place.
bool mul_fits(std::size_t a, std::size_t b) noexcept {
    if (a == 0 || b == 0) { return true; }
    return a <= std::numeric_limits<std::size_t>::max() / b;
}

bool dimension_ok(int value) noexcept { return value >= 1 && value <= kMaxRenderDimension; }

}  // namespace

std::size_t required_buffer_bytes(int width, int height, std::size_t stride_bytes) noexcept {
    if (!dimension_ok(width) || !dimension_ok(height)) { return 0; }

    // width <= kMaxRenderDimension (20000) and kRenderBytesPerPixel == 4, so
    // this product is at most 80000 — it cannot overflow. The cast is still
    // written explicitly because -Wsign-conversion is an error here and an
    // implicit int -> size_t on a value that has NOT yet been range-checked
    // is exactly the bug this module is trying not to have.
    const std::size_t row_bytes = static_cast<std::size_t>(width) * kRenderBytesPerPixel;

    const std::size_t stride = (stride_bytes == 0) ? row_bytes : stride_bytes;
    if (stride < row_bytes) {
        // Rows would overlap. PDFium would happily write with the stride it
        // is given and corrupt the caller's earlier rows.
        return 0;
    }
    if (stride % kRenderBytesPerPixel != 0) {
        // A stride that is not a whole number of 32-bit pixels makes every
        // row after the first start mid-pixel. PDFium does not tolerate it:
        // its FXDIB bitmaps assume 4-byte-aligned scanlines for a 32 bpp
        // format, and handing it an unaligned stride aborts the PROCESS on a
        // CHECK inside the library (observed: SIGTRAP, no diagnostic, on
        // stride = width*4 + 37). A library that kills the process on bad
        // input is not something to pass caller-controlled values to, so the
        // constraint is enforced here, where it is a Status.
        //
        // This costs callers nothing real: both platform bitmap formats this
        // path exists for are already aligned — Android's ARGB_8888 stride
        // and iOS's CGBitmapContext bytes-per-row are both multiples of 4.
        return 0;
    }

    const std::size_t rows = static_cast<std::size_t>(height);
    if (!mul_fits(stride, rows)) { return 0; }

    // stride * height, not stride * (height - 1) + row_bytes. The tighter
    // bound is correct for what PDFium actually writes, but it means a
    // caller who hands us a buffer sized to the tight bound has a target
    // whose last row has no trailing padding — and every later change to
    // this file would have to keep re-deriving that. Requiring the full
    // rectangle costs at most `stride - row_bytes` bytes once and makes the
    // safe size the obvious one.
    return stride * rows;
}

std::size_t RenderTarget::effective_stride() const noexcept {
    if (!dimension_ok(width)) { return 0; }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * kRenderBytesPerPixel;
    return (stride_bytes == 0) ? row_bytes : stride_bytes;
}

Status RenderTarget::validate() const noexcept {
    if (pixels == nullptr) { return Status::InvalidArgument; }

    const std::size_t needed = required_buffer_bytes(width, height, stride_bytes);
    if (needed == 0) {
        // required_buffer_bytes() folds every "this geometry is not usable"
        // case into 0: non-positive, over the cap, stride below the row
        // width, or an overflowing product. They are one class of caller
        // error and one status; splitting them would produce codes a caller
        // cannot act on differently.
        return Status::PdfRenderSizeInvalid;
    }
    if (capacity_bytes < needed) { return Status::PdfBufferTooSmall; }
    return Status::Ok;
}

}  // namespace uapp::pdf
