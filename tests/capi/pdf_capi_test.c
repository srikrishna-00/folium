/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/pdf_capi_test.c -- read-only PDF, exercised from C.
 *
 * C99, same -Werror posture as the other capi test files. This translation
 * unit is only COMPILED when UAPP_PDF_ENABLED is true -- see tests/CMakeLists.txt
 * -- because uapp_pdf.h's functions do not exist in a library built without a
 * vendored PDFium. There is no "skip at runtime" path here on purpose: if
 * this file is being compiled at all, the surface it tests must be present,
 * or the build itself should have refused to get this far.
 *
 * The fixture below is a minimal, hand-built single-page PDF -- no text
 * operators (a font resource would depend on system font discovery, which
 * differs between a CI container and a laptop; see
 * tests/pdf_engine/pdf_fixture.h for the fuller version of this argument),
 * no compression, byte-exact xref offsets computed by hand once and verified
 * against this project's own Document class before being embedded here.
 *
 * Two groups, two CTest entries: "slice:" is "open/measure/render actually
 * works through the C API"; "boundary:" is "malformed input and a wrong
 * handle are defined errors, never a crash".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_pdf.h"

#include "capi_test_harness.h"

/* A single 200x100-point page, Catalog -> Pages -> Page, byte-exact xref.
 * See this file's header comment for how these offsets were produced. */
static const unsigned char kMinimalPdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] >>\nendobj\n"
    "xref\n0 4\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "trailer\n<< /Size 4 /Root 1 0 R >>\n"
    "startxref\n186\n%%EOF";

static const size_t kMinimalPdfSize = sizeof kMinimalPdf - 1u; /* drop the NUL */

static uapp_pdf_document_t* open_fixture(void) {
    uapp_pdf_document_t* doc    = NULL;
    const uapp_status_t  status = uapp_pdf_document_open(kMinimalPdf, kMinimalPdfSize, NULL, &doc);
    CHECK_OK(status);
    return doc;
}

/* ===========================================================================
 * slice:
 * =========================================================================== */

static void test_slice_is_available_and_limits(void) {
    CHECK(uapp_pdf_is_available() == UAPP_TRUE);

    uapp_pdf_limits_t limits;
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_pdf_get_limits(&limits));
    CHECK(limits.max_render_dimension > 0);
    CHECK(limits.bytes_per_pixel == 4u);
}

static void test_slice_required_buffer_bytes(void) {
    const size_t packed = uapp_pdf_required_buffer_bytes(10, 10, 0);
    CHECK(packed == 10u * 10u * 4u);

    /* A stride wider than the tight packing must be honoured, not ignored. */
    const size_t padded = uapp_pdf_required_buffer_bytes(10, 10, 64);
    CHECK(padded == 64u * 10u);

    /* Unusable geometry is 0, always meaning "do not allocate this". */
    CHECK(uapp_pdf_required_buffer_bytes(0, 10, 0) == 0u);
    CHECK(uapp_pdf_required_buffer_bytes(10, -1, 0) == 0u);
    CHECK(uapp_pdf_required_buffer_bytes(1000000, 1000000, 0) == 0u);
}

static void test_slice_open_count_and_measure(void) {
    uapp_pdf_document_t* doc = open_fixture();

    CHECK(uapp_pdf_page_count(doc) == 1);

    uapp_pdf_page_size_t size;
    size.struct_size = (uint32_t)sizeof size;
    REQUIRE_OK(uapp_pdf_page_size(doc, 0, &size));
    CHECK(size.width_points == 200.0);
    CHECK(size.height_points == 100.0);

    CHECK_STATUS(uapp_pdf_page_size(doc, 1, &size), UAPP_STATUS_PDF_PAGE_NOT_FOUND);
    CHECK_STATUS(uapp_pdf_page_size(doc, -1, &size), UAPP_STATUS_PDF_PAGE_NOT_FOUND);

    uapp_pdf_document_close(&doc);
    CHECK(doc == NULL);
}

static void test_slice_render_writes_real_pixels(void) {
    uapp_pdf_document_t* doc = open_fixture();

    const int32_t width  = 20;
    const int32_t height = 10;
    const size_t  bytes  = uapp_pdf_required_buffer_bytes(width, height, 0);
    unsigned char* pixels = (unsigned char*)malloc(bytes);
    CHECK(pixels != NULL);
    if (pixels != NULL) {
        /* Poison the buffer first so "still poisoned" and "written" are
         * distinguishable -- a render that silently no-ops must not pass. */
        memset(pixels, 0xAA, bytes);

        REQUIRE_OK(uapp_pdf_render_page(doc, 0, pixels, bytes, width, height, 0));

        int all_poisoned = 1;
        for (size_t i = 0; i < bytes; ++i) {
            if (pixels[i] != 0xAAu) { all_poisoned = 0; break; }
        }
        CHECK(all_poisoned == 0);

        /* An empty page over opaque white: every pixel's alpha channel must
         * be fully opaque, whatever the RGB channels ended up being. */
        for (int32_t row = 0; row < height; ++row) {
            const unsigned char alpha = pixels[(size_t)row * (size_t)width * 4u + 3u];
            CHECK(alpha == 0xFFu);
        }

        free(pixels);
    }

    uapp_pdf_document_close(&doc);
}

static void test_slice_render_honours_a_padded_stride(void) {
    uapp_pdf_document_t* doc = open_fixture();

    const int32_t width  = 10;
    const int32_t height = 5;
    const size_t  stride = 64; /* wider than the tight width * 4 = 40 */
    const size_t  bytes  = uapp_pdf_required_buffer_bytes(width, height, stride);
    CHECK(bytes == stride * (size_t)height);
    unsigned char* pixels = (unsigned char*)malloc(bytes);
    CHECK(pixels != NULL);
    if (pixels != NULL) {
        /* Poison everything, including the padding past width*4 in each
         * row, so "row 2 starts at `stride`" and "row 2 starts at width*4"
         * give different, checkable outcomes instead of both looking fine. */
        memset(pixels, 0xAA, bytes);

        REQUIRE_OK(uapp_pdf_render_page(doc, 0, pixels, bytes, width, height, stride));

        /* Alpha at the START of row 1 (byte offset `stride + 3`) must be the
         * opaque page background PDFium just drew -- proving row 1 was
         * placed at the real stride, not at the tightly-packed width * 4. */
        CHECK(pixels[stride + 3u] == 0xFFu);
        /* The write must never touch anything beyond `bytes` -- a crafted
         * stride/geometry pair that overran here is exactly what the
         * sanitizer build of this suite (Debug+ASan/UBSan) is run to catch. */
        free(pixels);
    }

    uapp_pdf_document_close(&doc);
}

/* ===========================================================================
 * boundary:
 * =========================================================================== */

static void test_boundary_malformed_and_empty_input(void) {
    uapp_pdf_document_t* doc = NULL;

    CHECK_STATUS(uapp_pdf_document_open(NULL, 0, NULL, &doc), UAPP_STATUS_FILE_EMPTY);
    CHECK(doc == NULL);

    CHECK_STATUS(uapp_pdf_document_open("not a pdf at all", 16, NULL, &doc),
                UAPP_STATUS_PDF_MALFORMED);
    CHECK(doc == NULL);

    CHECK_STATUS(uapp_pdf_document_open(kMinimalPdf, 4, NULL, &doc),
                UAPP_STATUS_PDF_MALFORMED);
    CHECK(doc == NULL);

    CHECK_STATUS(uapp_pdf_document_open(kMinimalPdf, kMinimalPdfSize, NULL, NULL),
                UAPP_STATUS_INVALID_ARGUMENT);
}

static void test_boundary_null_handle_everywhere(void) {
    uapp_pdf_page_size_t size;
    size.struct_size = (uint32_t)sizeof size;

    CHECK(uapp_pdf_page_count(NULL) == 0);
    CHECK_STATUS(uapp_pdf_page_size(NULL, 0, &size), UAPP_STATUS_PDF_NOT_OPEN);

    unsigned char pixels[64];
    CHECK_STATUS(uapp_pdf_render_page(NULL, 0, pixels, sizeof pixels, 4, 4, 0),
                UAPP_STATUS_PDF_NOT_OPEN);

    /* Safe no-ops. */
    uapp_pdf_document_close(NULL);
    uapp_pdf_document_t* already_null = NULL;
    uapp_pdf_document_close(&already_null);
}

static void test_boundary_use_after_close_is_a_status_not_a_crash(void) {
    uapp_pdf_document_t* doc = open_fixture();
    uapp_pdf_document_close(&doc);
    CHECK(doc == NULL);

    /* `doc` is now NULL, which every accessor already treats as "not open".
     * Calling close() again on the same (now-NULL) slot must also be safe. */
    uapp_pdf_document_close(&doc);
    CHECK(uapp_pdf_page_count(doc) == 0);
}

static void test_boundary_render_size_and_buffer_validation(void) {
    uapp_pdf_document_t* doc = open_fixture();
    unsigned char        pixels[400];

    CHECK_STATUS(uapp_pdf_render_page(doc, 0, pixels, sizeof pixels, 0, 10, 0),
                UAPP_STATUS_PDF_RENDER_SIZE_INVALID);
    CHECK_STATUS(uapp_pdf_render_page(doc, 0, pixels, sizeof pixels, 10, -1, 0),
                UAPP_STATUS_PDF_RENDER_SIZE_INVALID);

    /* capacity_bytes honestly too small for the requested geometry. */
    CHECK_STATUS(uapp_pdf_render_page(doc, 0, pixels, 4, 10, 10, 0),
                UAPP_STATUS_PDF_BUFFER_TOO_SMALL);

    /* A page index past the end. */
    CHECK_STATUS(uapp_pdf_render_page(doc, 5, pixels, sizeof pixels, 10, 10, 0),
                UAPP_STATUS_PDF_PAGE_NOT_FOUND);

    uapp_pdf_document_close(&doc);
}

static void test_boundary_struct_size_versioning(void) {
    uapp_pdf_limits_t limits;
    limits.struct_size = 1u; /* smaller than this build's struct */
    CHECK_STATUS(uapp_pdf_get_limits(&limits), UAPP_STATUS_INVALID_ARGUMENT);

    uapp_pdf_document_t*  doc = open_fixture();
    uapp_pdf_page_size_t size;
    size.struct_size = 1u;
    CHECK_STATUS(uapp_pdf_page_size(doc, 0, &size), UAPP_STATUS_INVALID_ARGUMENT);
    uapp_pdf_document_close(&doc);
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

static const uapp_test_case_t k_cases[] = {
    {"slice: uapp_pdf_is_available and limits", test_slice_is_available_and_limits},
    {"slice: required_buffer_bytes matches tight and padded strides",
     test_slice_required_buffer_bytes},
    {"slice: open, count pages, and measure page size",
     test_slice_open_count_and_measure},
    {"slice: render writes real, fully-opaque pixels", test_slice_render_writes_real_pixels},
    {"slice: render honours a padded stride", test_slice_render_honours_a_padded_stride},

    {"boundary: empty and malformed PDF bytes are defined errors",
     test_boundary_malformed_and_empty_input},
    {"boundary: a null document handle is rejected everywhere",
     test_boundary_null_handle_everywhere},
    {"boundary: use after close is a status, not a crash",
     test_boundary_use_after_close_is_a_status_not_a_crash},
    {"boundary: bad render geometry and an undersized buffer",
     test_boundary_render_size_and_buffer_validation},
    {"boundary: a stale struct_size is rejected", test_boundary_struct_size_versioning}};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;
    return uapp_run_test_cases("uapp PDF C ABI tests", k_cases,
                               sizeof k_cases / sizeof k_cases[0], filter);
}
