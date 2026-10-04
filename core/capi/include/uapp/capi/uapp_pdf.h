/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_pdf.h -- the PDF VIEWING surface of libuapp_capi.
 *
 * A one-to-one C99 face over pdf-engine/reader (uapp::pdf::Document): open a
 * PDF from a buffer, count pages, measure a page, render a page into a
 * caller-owned buffer. Spec section 11 lists ~35 PDF features; this is the
 * same four pdf-engine/reader itself implements, no more -- editing, merge/
 * split, forms, annotation and signatures are NOT IMPLEMENTED at every layer
 * of this stack, not just this one.
 *
 * ===========================================================================
 * THIS SURFACE IS OPTIONAL. CHECK uapp_pdf_is_available() BEFORE USE.
 * ===========================================================================
 * pdf-engine/ is built only when a PDFium is vendored (UAPP_ENABLE_PDF, AUTO
 * by default -- see the root CMakeLists.txt and third_party/pdfium/README.md).
 * A library built without it does not export any uapp_pdf_* symbol declared
 * below; calling one is a link error against a static archive and a
 * missing-symbol abort against a shared one. uapp_pdf_is_available(), declared
 * in uapp_capi.h because it is the one symbol that must always exist to
 * answer this question, is how a binding finds out before it tries.
 *
 * ===========================================================================
 * THE LIBRARY-LIFECYCLE DESIGN CHOICE: NO SEPARATE Library HANDLE
 * ===========================================================================
 * The C++ layer has two types: Library (a refcounted wrapper over PDFium's
 * process-global FPDF_InitLibrary/FPDF_DestroyLibrary) and Document (which
 * holds one Library member automatically). A caller that never mentions
 * Library still gets correct init/destroy pairing; a caller opening many
 * documents in a loop can hold one explicitly to keep PDFium initialised
 * across the whole loop instead of paying init/destroy per document.
 *
 * This header exposes ONLY Document, not Library, and that is a deliberate
 * narrowing, not an oversight:
 *
 *   - A mobile binding opens a handful of PDFs per session, not thousands in
 *     a tight loop. The per-document init/destroy cost Library's explicit
 *     form exists to amortise is not a cost a document viewer ever pays
 *     often enough to optimise for.
 *   - Every other handle in this ABI maps to exactly one C++ RAII type.
 *     Adding a second handle whose entire job is "make the first one faster
 *     in a case that does not arise here" is API surface a Kotlin/Swift
 *     binding would have to bind, document and get the lifetime of right,
 *     for a benefit this product does not need yet.
 *   - If a caller does open many PDFs back to back and the init/destroy cost
 *     is measured and shown to matter, uapp_pdf_library_t can be added later
 *     as a genuinely additive surface -- nothing about exposing it now versus
 *     later changes Document's own ABI.
 *
 * So: uapp_pdf_document_open() pays PDFium's init cost on the first open in
 * the process and on every open after the last document closes, exactly as
 * uapp::pdf::Document does internally. This is documented here as a NOTED
 * TRADEOFF, not a silent one.
 *
 * ERRORS, OWNERSHIP, THREADING: uapp_capi.h's rules, unchanged.
 */

#ifndef UAPP_CAPI_UAPP_PDF_H
#define UAPP_CAPI_UAPP_PDF_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- limits ---------------------------------------------------------------
 *
 * Read at runtime for the reason every other limits struct in this ABI is:
 * a client that hard-codes a bound the loaded library does not enforce shows
 * the user a validation message that lies.
 */
typedef struct uapp_pdf_limits {
    uint32_t struct_size;          /* in: sizeof(uapp_pdf_limits_t) */
    int32_t  max_render_dimension; /* out: per-side pixel cap; see document.h */
    uint32_t bytes_per_pixel;      /* out: == 4 (RGBA8888, the only format produced) */
} uapp_pdf_limits_t;

UAPP_CAPI_API uapp_status_t uapp_pdf_get_limits(uapp_pdf_limits_t* out_limits);

/*
 * Minimum buffer size, in bytes, for a render of `width` x `height` at
 * `stride_bytes` (0 meaning "tightly packed", i.e. width * 4). 0 means the
 * geometry is unusable -- non-positive, over the limit, or arithmetically
 * overflowing -- and ALWAYS means "do not allocate this", never "zero bytes
 * needed". A caller sizing a pixel buffer before opening any document calls
 * this directly; it does not need a uapp_pdf_document_t.
 */
UAPP_CAPI_API size_t uapp_pdf_required_buffer_bytes(int32_t width, int32_t height,
                                                    size_t stride_bytes);

/* --- page size ------------------------------------------------------------ */

typedef struct uapp_pdf_page_size {
    uint32_t struct_size;    /* in: sizeof(uapp_pdf_page_size_t) */
    double   width_points;   /* out: PDF user-space units, 1/72 inch */
    double   height_points;  /* out */
} uapp_pdf_page_size_t;

/* --- the document handle --------------------------------------------------
 *
 * Opaque; validated by magic cookie like every other handle in this ABI. See
 * uapp_capi.h's handle rules for the null/zeroed/wrong-type/use-after-close
 * cases this catches and the one it cannot (true use-after-free).
 */
typedef struct uapp_pdf_document uapp_pdf_document_t;

/*
 * Open a PDF held entirely in memory.
 *
 * ===========================================================================
 * `data` MUST STAY VALID, UNMODIFIED AND AT THE SAME ADDRESS FOR THE LIFETIME
 * OF THE RETURNED DOCUMENT HANDLE.
 * ===========================================================================
 * This is not a convenience API that copies your bytes: PDFium parses lazily
 * out of the caller's buffer for the document's whole life, exactly as
 * document.h documents for the C++ layer this wraps, and copying here would
 * double peak memory for a large PDF. Freeing or mutating `data` before
 * calling uapp_pdf_document_close() is a use-after-free inside PDFium that
 * nothing in this ABI can detect. A Kotlin/Swift binding MUST pin the backing
 * buffer (a direct ByteBuffer, an NSData that is not autoreleased) for the
 * handle's whole life.
 *
 * `password` may be NULL for an unencrypted document. Passed to PDFium as a
 * NUL-terminated byte string; an embedded NUL in a non-NULL password is
 * rejected outright (UAPP_STATUS_INVALID_ARGUMENT) rather than silently
 * truncated at the NUL, matching the C++ layer's documented behaviour.
 *
 * `out_document` is written ONLY on UAPP_STATUS_OK; a failed open never
 * leaves a handle that must be closed.
 *
 * Returns:
 *   UAPP_STATUS_OK
 *   UAPP_STATUS_INVALID_ARGUMENT        -- NULL data/out_document with a
 *                                          non-zero size, or NUL inside
 *                                          `password`
 *   UAPP_STATUS_FILE_EMPTY              -- size is 0
 *   UAPP_STATUS_PDF_MALFORMED           -- not a PDF, or damaged past recovery
 *   UAPP_STATUS_PDF_PASSWORD_REQUIRED   -- encrypted; absent or wrong password
 *   UAPP_STATUS_PDF_SECURITY_UNSUPPORTED
 *   UAPP_STATUS_OUT_OF_MEMORY           -- the handle itself could not be allocated
 */
UAPP_CAPI_API uapp_status_t uapp_pdf_document_open(const void* data, size_t size,
                                                   const char* password,
                                                   uapp_pdf_document_t** out_document);

/*
 * Close and free `*document`, then set `*document` to NULL. Safe to call with
 * `document == NULL` or `*document == NULL` (a no-op). Idempotent: calling it
 * twice on the same slot is safe because the first call nulls the slot.
 *
 * Does NOT free or otherwise touch the `data` buffer passed to
 * uapp_pdf_document_open() -- that memory was always the caller's.
 */
UAPP_CAPI_API void uapp_pdf_document_close(uapp_pdf_document_t** document);

/*
 * Pages in the document. 0 for an invalid/closed handle AND for a validly
 * open document whose page tree PDFium could not read (a real PDF always has
 * at least one page, so 0 from an open document is itself informative -- the
 * next call to uapp_pdf_page_size()/uapp_pdf_render_page() will report the
 * specific failure). Cannot fail; there is no status to return.
 */
UAPP_CAPI_API int32_t uapp_pdf_page_count(const uapp_pdf_document_t* document);

/*
 * Intrinsic size of page `index` in points. Does not rasterise.
 *
 * `out_size` MUST have struct_size set first; written only on
 * UAPP_STATUS_OK.
 *
 * Returns UAPP_STATUS_OK, UAPP_STATUS_INVALID_ARGUMENT (NULL out_size or bad
 * struct_size), UAPP_STATUS_PDF_NOT_OPEN, UAPP_STATUS_PDF_PAGE_NOT_FOUND, or
 * UAPP_STATUS_PDF_PAGE_LOAD_FAILED.
 */
UAPP_CAPI_API uapp_status_t uapp_pdf_page_size(const uapp_pdf_document_t* document,
                                               int32_t index,
                                               uapp_pdf_page_size_t* out_size);

/*
 * Rasterise page `index` into the CALLER-OWNED buffer `pixels`, scaled to
 * fill exactly `width` x `height` -- aspect ratio is NOT preserved, because
 * the caller (who already knows the page's intrinsic size from
 * uapp_pdf_page_size()) is the only party that knows whether it wants
 * letterboxing, cropping or a stretch.
 *
 * ===========================================================================
 * ZERO-COPY, STRUCTURALLY: THIS FUNCTION NEVER ALLOCATES A PIXEL BUFFER
 * ===========================================================================
 * `pixels` is written in place. On Android, pass the address
 * AndroidBitmap_lockPixels() returns for the destination Bitmap; on iOS, pass
 * CGBitmapContextGetData(). Neither platform's buffer is copied by this call.
 * This is the entire reason document.h's RenderTarget takes a caller buffer
 * instead of returning one, and this ABI preserves that contract exactly --
 * adding a copy here that the C++ layer refuses to do would silently reopen
 * the budget SP-5's findings closed structurally.
 *
 * `stride_bytes` is the distance between the start of consecutive rows, in
 * bytes. Pass 0 for "tightly packed" (width * 4); otherwise it must be >=
 * width * 4 AND A MULTIPLE OF 4 -- PDFium's 32bpp bitmaps assume 4-byte-
 * aligned scanlines and this is enforced as a status rather than trusted. A
 * platform bitmap's own stride (AndroidBitmapInfo::stride,
 * CGBitmapContextGetBytesPerRow) is always a valid value here; pass it
 * directly rather than computing width * 4 yourself, since platform strides
 * are frequently padded.
 *
 * Output format is RGBA8888, 8 bits per channel, R-G-B-A at ascending byte
 * addresses -- Android's ARGB_8888 and Skia's kRGBA_8888_SkColorType layout.
 * The buffer is filled with opaque white before the page is drawn, because a
 * PDF page has no background of its own.
 *
 * `capacity_bytes` is the number of bytes actually addressable from `pixels`;
 * use uapp_pdf_required_buffer_bytes() to compute the minimum before
 * allocating. Use it honestly -- this is the field the bounds check runs
 * against, so overstating it defeats the one thing standing between a
 * crafted PDF and an out-of-bounds write.
 *
 * Returns UAPP_STATUS_OK, UAPP_STATUS_PDF_NOT_OPEN,
 * UAPP_STATUS_PDF_PAGE_NOT_FOUND, UAPP_STATUS_PDF_PAGE_LOAD_FAILED,
 * UAPP_STATUS_PDF_RENDER_SIZE_INVALID (non-positive/oversized/overflowing
 * geometry), UAPP_STATUS_PDF_BUFFER_TOO_SMALL, or
 * UAPP_STATUS_PDF_RENDER_FAILED (PDFium refused the target). On any failure
 * the buffer's contents are unspecified, but every written byte stayed
 * inside `capacity_bytes`.
 */
UAPP_CAPI_API uapp_status_t uapp_pdf_render_page(const uapp_pdf_document_t* document,
                                                 int32_t index, void* pixels,
                                                 size_t capacity_bytes, int32_t width,
                                                 int32_t height, size_t stride_bytes);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_PDF_H */
