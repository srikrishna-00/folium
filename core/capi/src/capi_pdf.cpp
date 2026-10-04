// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_pdf.cpp — the C99 face of uapp::pdf::Document.
//
// This translation unit is compiled only when UAPP_PDF_ENABLED is set by the
// root CMakeLists.txt (i.e. only when pdf-engine/ itself is built, which
// requires a vendored PDFium). uapp_pdf_is_available() is NOT defined here —
// it lives in capi_common.cpp, which is always built, so a caller can ask the
// question before knowing whether this file's symbols exist at all. See the
// long note on that function in uapp_capi.h.
//
// House rules: same four as capi_notes.cpp's file header, unchanged.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_pdf.h"
#include "uapp/pdf/document.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::capi::to_c_status;
using uapp::pdf::Document;
using uapp::pdf::kMaxRenderDimension;
using uapp::pdf::kRenderBytesPerPixel;
using uapp::pdf::PageSize;
using uapp::pdf::RenderTarget;
using uapp::pdf::required_buffer_bytes;

// Distinct from every other magic value in capi_internal.h for the reason
// given there: a generated binding's most likely mistake is passing the
// wrong opaque pointer, and every handle in this ABI is an indistinguishable
// integer on the Kotlin/Swift side.
inline constexpr std::uint32_t kPdfDocumentMagic = 0x55415046u;  // 'UAPF'

template <typename Handle>
[[nodiscard]] bool handle_valid(const Handle* handle, std::uint32_t expected) noexcept {
    return handle != nullptr && handle->magic == expected;
}

}  // namespace

// --- handle definition -------------------------------------------------------

struct uapp_pdf_document final {
    std::uint32_t magic = kPdfDocumentMagic;
    Document      doc;
};

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

uapp_status_t uapp_pdf_get_limits(uapp_pdf_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (out_limits->struct_size < sizeof(uapp_pdf_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->max_render_dimension = static_cast<std::int32_t>(kMaxRenderDimension);
        out_limits->bytes_per_pixel      = static_cast<std::uint32_t>(kRenderBytesPerPixel);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

size_t uapp_pdf_required_buffer_bytes(int32_t width, int32_t height, size_t stride_bytes) {
    UAPP_CAPI_GUARD_BEGIN
        return required_buffer_bytes(width, height, stride_bytes);
    UAPP_CAPI_GUARD_END_FALLBACK(0u)
}

uapp_status_t uapp_pdf_document_open(const void* data, size_t size, const char* password,
                                     uapp_pdf_document_t** out_document) {
    UAPP_CAPI_GUARD_BEGIN
        // No `*out_document = nullptr` prologue: out-parameters are not
        // written on a failure path anywhere in this ABI (see
        // capi_notes.cpp's note on uapp_note_service_open for the full
        // argument). Leaving the caller's slot untouched on failure is one
        // rule for the whole codebase instead of a PDF-specific exception.
        if (out_document == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (data == nullptr && size != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        std::unique_ptr<uapp_pdf_document> handle(new (std::nothrow) uapp_pdf_document());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        const auto* bytes = static_cast<const std::uint8_t*>(data);
        Status      opened;
        if (password != nullptr) {
            opened = Document::open_buffer(bytes, size, std::string_view(password), handle->doc);
        } else {
            opened = Document::open_buffer(bytes, size, handle->doc);
        }
        if (!uapp::is_ok(opened)) {
            return to_c_status(opened);
        }

        *out_document = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_pdf_document_close(uapp_pdf_document_t** document) {
    UAPP_CAPI_GUARD_BEGIN
        if (document == nullptr || *document == nullptr) {
            return;
        }
        if (!handle_valid(*document, kPdfDocumentMagic)) {
            // Not ours, or already scrubbed. Deleting it would be worse than
            // leaking it, and there is no channel left to report on.
            return;
        }
        (*document)->magic = 0u;
        delete *document;
        *document = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

int32_t uapp_pdf_page_count(const uapp_pdf_document_t* document) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(document, kPdfDocumentMagic)) {
            return 0;
        }
        return document->doc.page_count();
    UAPP_CAPI_GUARD_END_FALLBACK(0)
}

uapp_status_t uapp_pdf_page_size(const uapp_pdf_document_t* document, int32_t index,
                                 uapp_pdf_page_size_t* out_size) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_size == nullptr || out_size->struct_size < sizeof(uapp_pdf_page_size_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(document, kPdfDocumentMagic)) {
            return UAPP_STATUS_PDF_NOT_OPEN;
        }

        PageSize     size;
        const Status status = document->doc.page_size(index, size);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        out_size->width_points  = size.width_points;
        out_size->height_points = size.height_points;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_pdf_render_page(const uapp_pdf_document_t* document, int32_t index,
                                   void* pixels, size_t capacity_bytes, int32_t width,
                                   int32_t height, size_t stride_bytes) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(document, kPdfDocumentMagic)) {
            return UAPP_STATUS_PDF_NOT_OPEN;
        }

        RenderTarget target;
        target.pixels         = static_cast<std::uint8_t*>(pixels);
        target.capacity_bytes = capacity_bytes;
        target.width          = width;
        target.height         = height;
        target.stride_bytes   = stride_bytes;

        const Status status = document->doc.render_page(index, target);
        return uapp::is_ok(status) ? UAPP_STATUS_OK : to_c_status(status);
    UAPP_CAPI_GUARD_END
}

}  // extern "C"
