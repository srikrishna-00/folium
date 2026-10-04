/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/file_capi_test.c -- file-type detection, exercised from C.
 *
 * C99, -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Werror, same posture
 * as the other capi test files. Includes only the public uapp_file.h and
 * uapp_capi.h, matching uapp_file.h's own claim that this is the one surface
 * in the ABI with no handle and no state to set up.
 *
 * Two groups, two CTest entries: "slice:" proves detection actually works
 * through the C API for a few real formats and the ZIP-family ambiguity;
 * "boundary:" proves malformed/adversarial input does not crash it.
 */

#include <stdio.h>
#include <string.h>

#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_file.h"

#include "capi_test_harness.h"

/* --- fixtures --------------------------------------------------------------
 *
 * Minimal real signatures, not full files -- detection reads bounded head/
 * tail windows and these fixtures are sized to stay inside them. The ZIP
 * fixtures carry a real central directory, because that is the one thing
 * local-header-only bytes cannot prove: DOCX/XLSX/PPTX are told apart by
 * which specific member is present in the AUTHORITATIVE member list, not by
 * the ZIP magic alone.
 */

static const unsigned char kPngBytes[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                          0x00, 0x00, 0x00, 0x00};

static const unsigned char kJpegBytes[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x00};

static const unsigned char kPdfBytes[] = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

static const char kPlainText[] =
    "The quick brown fox jumps over the lazy dog. Plain ASCII prose, long "
    "enough to clear the text-heuristic sample window with margin.";

/* A ZIP with ONE central-directory record naming "word/document.xml" -- the
 * DOCX tell. Built by hand rather than with a real zip tool, for the same
 * determinism argument tests/pdf_engine/pdf_fixture.h gives for PDF: no
 * timestamps, no compression, the same bytes every run.
 *
 * Layout: one STORED (uncompressed) local file header + name + 1-byte body,
 * one matching central-directory header, one EOCD record pointing at the
 * central directory's real offset.
 */
static size_t build_docx_like_zip(unsigned char* out, size_t out_capacity,
                                  const char* member_name) {
    const size_t name_len = strlen(member_name);
    size_t       pos      = 0;

    size_t lfh_offset = pos;
    /* local file header */
    unsigned char lfh[30] = {
        'P', 'K', 0x03, 0x04, /* signature */
        20, 0, 0, 0,          /* version needed, flags */
        0, 0,                 /* compression = stored */
        0, 0, 0, 0,           /* mod time/date */
        0, 0, 0, 0,           /* crc32 (not checked by detection) */
        1, 0, 0, 0,           /* compressed size = 1 */
        1, 0, 0, 0,           /* uncompressed size = 1 */
        0, 0,                 /* name length (patched below) */
        0, 0                  /* extra length */
    };
    lfh[26] = (unsigned char)(name_len & 0xFFu);
    lfh[27] = (unsigned char)((name_len >> 8) & 0xFFu);
    if (pos + sizeof lfh + name_len + 1 > out_capacity) { return 0; }
    memcpy(out + pos, lfh, sizeof lfh);
    pos += sizeof lfh;
    memcpy(out + pos, member_name, name_len);
    pos += name_len;
    out[pos] = 'x'; /* the one-byte "file content" */
    pos += 1;

    size_t cd_offset = pos;
    unsigned char cdh[46] = {
        'P', 'K', 0x01, 0x02, /* signature */
        20, 0, 20, 0,         /* version made by, version needed */
        0, 0,                 /* flags */
        0, 0,                 /* compression = stored */
        0, 0, 0, 0,           /* mod time/date */
        0, 0, 0, 0,           /* crc32 */
        1, 0, 0, 0,           /* compressed size */
        1, 0, 0, 0,           /* uncompressed size */
        0, 0,                 /* name length (patched below) */
        0, 0,                 /* extra length */
        0, 0,                 /* comment length */
        0, 0,                 /* disk number start */
        0, 0,                 /* internal attrs */
        0, 0, 0, 0,           /* external attrs */
        0, 0, 0, 0            /* local header offset (patched below) */
    };
    cdh[28] = (unsigned char)(name_len & 0xFFu);
    cdh[29] = (unsigned char)((name_len >> 8) & 0xFFu);
    cdh[42] = (unsigned char)(lfh_offset & 0xFFu);
    cdh[43] = (unsigned char)((lfh_offset >> 8) & 0xFFu);
    if (pos + sizeof cdh + name_len > out_capacity) { return 0; }
    memcpy(out + pos, cdh, sizeof cdh);
    pos += sizeof cdh;
    memcpy(out + pos, member_name, name_len);
    pos += name_len;

    size_t cd_size = pos - cd_offset;
    unsigned char eocd[22] = {
        'P', 'K', 0x05, 0x06, /* signature */
        0, 0, 0, 0,           /* disk numbers */
        1, 0, 1, 0,           /* entries on this disk, total entries */
        0, 0, 0, 0,           /* central directory size (patched below) */
        0, 0, 0, 0,           /* central directory offset (patched below) */
        0, 0                  /* comment length */
    };
    eocd[12] = (unsigned char)(cd_size & 0xFFu);
    eocd[13] = (unsigned char)((cd_size >> 8) & 0xFFu);
    eocd[16] = (unsigned char)(cd_offset & 0xFFu);
    eocd[17] = (unsigned char)((cd_offset >> 8) & 0xFFu);
    if (pos + sizeof eocd > out_capacity) { return 0; }
    memcpy(out + pos, eocd, sizeof eocd);
    pos += sizeof eocd;

    return pos;
}

/* ===========================================================================
 * slice:
 * =========================================================================== */

static void test_slice_registry_is_queryable(void) {
    uapp_file_limits_t limits;
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_file_get_limits(&limits));
    CHECK(limits.format_count >= 10u);
    CHECK(limits.implemented_format_count == 10u);
    CHECK(limits.capability_count == 10u);

    CHECK(uapp_file_format_count() == limits.format_count);
    CHECK(uapp_file_format_detection_is_implemented(UAPP_FILE_FORMAT_PDF) == UAPP_TRUE);
    CHECK(uapp_file_format_detection_is_implemented(UAPP_FILE_FORMAT_MP3) == UAPP_FALSE);
    /* The pair this module exists to distinguish: UNKNOWN from "sniffed and
     * missed" is different from UNKNOWN from "never taught this format". */
    CHECK(uapp_file_format_detection_not_implemented_reason(UAPP_FILE_FORMAT_PDF) == NULL);
    CHECK(uapp_file_format_detection_not_implemented_reason(UAPP_FILE_FORMAT_MP3) != NULL);

    CHECK_STR(uapp_file_format_name(UAPP_FILE_FORMAT_DOCX), "docx");
    CHECK_STR(uapp_file_format_mime_type(UAPP_FILE_FORMAT_PNG), "image/png");
    CHECK(uapp_file_format_category(UAPP_FILE_FORMAT_DOCX) == UAPP_FORMAT_CATEGORY_DOCUMENT);
}

static void test_slice_capabilities_are_claims_from_the_matrix(void) {
    const uint32_t pdf_caps = uapp_file_format_capabilities(UAPP_FILE_FORMAT_PDF);
    CHECK(uapp_file_capabilities_have(pdf_caps, UAPP_FILE_CAPABILITY_VIEW) == UAPP_TRUE);
    /* SIGN is granted to nothing in this build, including PDF -- a wrong
     * claim here is a UI bug waiting to happen, so it is asserted directly
     * rather than trusted. */
    CHECK(uapp_file_capabilities_have(pdf_caps, UAPP_FILE_CAPABILITY_SIGN) == UAPP_FALSE);

    const uint32_t unknown_caps = uapp_file_format_capabilities(UAPP_FILE_FORMAT_UNKNOWN);
    CHECK(unknown_caps == 0u);

    /* A mask, not exactly one bit, must never be reported "present". */
    CHECK(uapp_file_capabilities_have(pdf_caps, (uapp_file_capability_t)0) == UAPP_FALSE);
}

static void test_slice_detect_buffer_real_formats(void) {
    uapp_file_info_t info;

    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_detect_buffer(kPngBytes, sizeof kPngBytes, "photo.png", &info));
    CHECK(info.format == UAPP_FILE_FORMAT_PNG);
    CHECK(info.confidence == UAPP_DETECTION_CONFIDENCE_MAGIC);
    CHECK(info.extension_conflicts == UAPP_FALSE);
    CHECK(info.extension_present == UAPP_TRUE);
    CHECK(info.extension_matches_content == UAPP_TRUE);
    CHECK_STR(info.mime_type, "image/png");

    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_detect_buffer(kJpegBytes, sizeof kJpegBytes, NULL, &info));
    CHECK(info.format == UAPP_FILE_FORMAT_JPEG);
    /* No name hint given at all -- distinct from a name that disagreed. */
    CHECK(info.extension_present == UAPP_FALSE);
    CHECK(info.extension_matches_content == UAPP_FALSE);

    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_detect_buffer(kPdfBytes, sizeof kPdfBytes - 1, "doc.pdf", &info));
    CHECK(info.format == UAPP_FILE_FORMAT_PDF);
    CHECK(info.method == UAPP_DETECTION_METHOD_MAGIC_SIGNATURE);

    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(
        uapp_file_detect_buffer(kPlainText, sizeof kPlainText - 1, "notes.txt", &info));
    CHECK(info.format == UAPP_FILE_FORMAT_PLAIN_TEXT);
    CHECK(info.confidence == UAPP_DETECTION_CONFIDENCE_HEURISTIC);
}

static void test_slice_renamed_file_sets_extension_conflicts(void) {
    uapp_file_info_t info;
    REQUIRE_OK(uapp_file_info_init(&info));
    /* A ZIP (well, enough of one) wearing a .jpg name. This is the one
     * signal worth showing a human before they open an attachment. */
    const unsigned char zip_magic[] = {'P', 'K', 0x03, 0x04, 0, 0, 0, 0};
    REQUIRE_OK(uapp_file_detect_buffer(zip_magic, sizeof zip_magic, "invoice.jpg", &info));
    CHECK(info.format != UAPP_FILE_FORMAT_JPEG);
    CHECK(info.extension_hint == UAPP_FILE_FORMAT_JPEG);
    CHECK(info.extension_conflicts == UAPP_TRUE);
    CHECK(info.extension_matches_content == UAPP_FALSE);
}

static void test_slice_zip_family_disambiguation(void) {
    unsigned char buf[512];
    uapp_file_info_t info;

    size_t n = build_docx_like_zip(buf, sizeof buf, "word/document.xml");
    CHECK(n > 0u);
    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_detect_buffer(buf, n, NULL, &info));
    CHECK(info.format == UAPP_FILE_FORMAT_DOCX);
    CHECK(info.container_format == UAPP_FILE_FORMAT_ZIP);
    CHECK(info.method == UAPP_DETECTION_METHOD_ZIP_CENTRAL_DIRECTORY);
    CHECK(info.container_truncated == UAPP_FALSE);

    n = build_docx_like_zip(buf, sizeof buf, "xl/workbook.xml");
    CHECK(n > 0u);
    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_detect_buffer(buf, n, NULL, &info));
    CHECK(info.format == UAPP_FILE_FORMAT_XLSX);

    n = build_docx_like_zip(buf, sizeof buf, "some/unrecognised/member.bin");
    CHECK(n > 0u);
    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_detect_buffer(buf, n, NULL, &info));
    /* Not an OOXML/ODF member and not a recognised family: an honest ZIP. */
    CHECK(info.format == UAPP_FILE_FORMAT_ZIP);
}

static void test_slice_describe_buffer_adds_checksum_and_size(void) {
    uapp_file_info_t info;
    REQUIRE_OK(uapp_file_info_init(&info));
    REQUIRE_OK(uapp_file_describe_buffer(kPlainText, sizeof kPlainText - 1, "notes.txt", 1000,
                                        2000, &info));
    CHECK(info.format == UAPP_FILE_FORMAT_PLAIN_TEXT);
    CHECK(info.size_bytes == sizeof kPlainText - 1u);
    CHECK(info.checksum_algorithm == UAPP_CHECKSUM_ALGORITHM_FNV1A64);
    CHECK(info.checksum_length == 8u);
    CHECK(info.location_kind == UAPP_FILE_LOCATION_MEMORY_BUFFER);

    /* detect_buffer on the SAME bytes must report NO checksum -- the whole
     * reason to prefer describe_* is paying for a digest only when asked. */
    uapp_file_info_t detect_only;
    REQUIRE_OK(uapp_file_info_init(&detect_only));
    REQUIRE_OK(uapp_file_detect_buffer(kPlainText, sizeof kPlainText - 1, "notes.txt",
                                       &detect_only));
    CHECK(detect_only.checksum_algorithm == UAPP_CHECKSUM_ALGORITHM_NONE);
    CHECK(detect_only.checksum_length == 0u);
}

static void test_slice_copy_extension(void) {
    char* ext = NULL;
    REQUIRE_OK(uapp_file_copy_extension("archive.tar.gz", &ext));
    CHECK_STR(ext, "gz");
    uapp_free_string(&ext);
    CHECK(ext == NULL);

    REQUIRE_OK(uapp_file_copy_extension(".bashrc", &ext));
    CHECK_STR(ext, ""); /* a dotfile has no extension */
    uapp_free_string(&ext);

    REQUIRE_OK(uapp_file_copy_extension("README", &ext));
    CHECK_STR(ext, "");
    uapp_free_string(&ext);
}

/* ===========================================================================
 * boundary:
 * =========================================================================== */

static void test_boundary_empty_and_short_buffers(void) {
    uapp_file_info_t info;
    REQUIRE_OK(uapp_file_info_init(&info));

    /* Empty is FILE_EMPTY, not a silent "no match" -- "there is nothing
     * here" and "there is something here we do not recognise" are different
     * facts and the ABI must not conflate them. */
    CHECK_STATUS(uapp_file_detect_buffer(NULL, 0, NULL, &info), UAPP_STATUS_FILE_EMPTY);
    CHECK_STATUS(uapp_file_detect_buffer((const void*)"", 0, NULL, &info),
                UAPP_STATUS_FILE_EMPTY);

    /* A 1-3 byte buffer is a normal OK result (format UNKNOWN), never a
     * crash or an error -- every signature comparison is bounds-checked. */
    const unsigned char tiny[] = {0x89};
    REQUIRE_OK(uapp_file_detect_buffer(tiny, sizeof tiny, NULL, &info));
    CHECK(info.format == UAPP_FILE_FORMAT_UNKNOWN);
}

static void test_boundary_null_and_bad_struct_size(void) {
    uapp_file_info_t info;
    REQUIRE_OK(uapp_file_info_init(&info));

    CHECK_STATUS(uapp_file_detect_buffer(kPngBytes, sizeof kPngBytes, NULL, NULL),
                UAPP_STATUS_INVALID_ARGUMENT);
    /* NULL data with a NON-ZERO size is a lie about readable memory, and
     * must not be silently treated as an empty file. */
    CHECK_STATUS(uapp_file_detect_buffer(NULL, 10, NULL, &info),
                UAPP_STATUS_INVALID_ARGUMENT);

    info.struct_size = 1u; /* smaller than this build's struct */
    CHECK_STATUS(uapp_file_detect_buffer(kPngBytes, sizeof kPngBytes, NULL, &info),
                UAPP_STATUS_INVALID_ARGUMENT);

    CHECK_STATUS(uapp_file_detect_path(NULL, &info), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_file_get_limits(NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_file_copy_extension(NULL, NULL), UAPP_STATUS_INVALID_ARGUMENT);
}

static void test_boundary_nonexistent_path(void) {
    uapp_file_info_t info;
    REQUIRE_OK(uapp_file_info_init(&info));
    CHECK_STATUS(uapp_file_detect_path("/no/such/path/ever", &info),
                UAPP_STATUS_FILE_OPEN_FAILED);
    CHECK_STATUS(uapp_file_detect_path("", &info), UAPP_STATUS_FILE_PATH_INVALID);
}

static void test_boundary_out_of_range_enum_values_are_total(void) {
    /* Every registry query must answer, not crash, for an integer that is
     * not a format/capability/confidence/method at all. */
    const uapp_file_format_t bogus = (uapp_file_format_t)123456789;
    CHECK_STR(uapp_file_format_name(bogus), "unknown");
    CHECK(uapp_file_format_category(bogus) == UAPP_FORMAT_CATEGORY_UNKNOWN);
    CHECK_STR(uapp_file_format_mime_type(bogus), "application/octet-stream");
    CHECK(uapp_file_format_detection_is_implemented(bogus) == UAPP_FALSE);
    /* A value that is not a format at all genuinely has no sniffer, so this
     * returns a reason, not NULL -- NULL is reserved for a format that IS
     * implemented (see UAPP_FILE_FORMAT_UNKNOWN's own category-error message
     * for the one other non-NULL case this function distinguishes). */
    CHECK(uapp_file_format_detection_not_implemented_reason(bogus) != NULL);
    CHECK(uapp_file_format_capabilities(bogus) == 0u);

    CHECK_STR(uapp_file_capability_name((uapp_file_capability_t)0), "unknown");
    CHECK_STR(uapp_detection_confidence_name((uapp_detection_confidence_t)999), "unknown");
    CHECK_STR(uapp_detection_method_name((uapp_detection_method_t)999), "unknown");
    CHECK_STR(uapp_checksum_algorithm_name((uapp_checksum_algorithm_t)999), "unknown");
    CHECK_STR(uapp_file_location_kind_name((uapp_file_location_kind_t)999), "unknown");

    CHECK(uapp_file_format_at((size_t)-1) == UAPP_FILE_FORMAT_UNKNOWN);
    CHECK(uapp_file_capability_at((uint32_t)-1) == UAPP_FILE_CAPABILITY_VIEW);
}

static void test_boundary_extension_hint_is_advisory_only(void) {
    /* format_from_extension must never be mistaken for detection: it is a
     * hint table, consulted by nothing in this file's detect/describe path
     * to set `format`. */
    CHECK(uapp_file_format_from_extension("pdf") == UAPP_FILE_FORMAT_PDF);
    CHECK(uapp_file_format_from_extension(".PDF") == UAPP_FILE_FORMAT_PDF);
    CHECK(uapp_file_format_from_extension(NULL) == UAPP_FILE_FORMAT_UNKNOWN);
    CHECK(uapp_file_format_from_extension("") == UAPP_FILE_FORMAT_UNKNOWN);
    CHECK(uapp_file_format_from_extension("not-a-real-extension") ==
         UAPP_FILE_FORMAT_UNKNOWN);
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

static const uapp_test_case_t k_cases[] = {
    {"slice: the format/capability registry is queryable", test_slice_registry_is_queryable},
    {"slice: capabilities are claims from the support matrix",
     test_slice_capabilities_are_claims_from_the_matrix},
    {"slice: detect_buffer identifies real formats from bytes",
     test_slice_detect_buffer_real_formats},
    {"slice: a renamed file sets extension_conflicts, not format",
     test_slice_renamed_file_sets_extension_conflicts},
    {"slice: DOCX/XLSX are told apart by the ZIP central directory",
     test_slice_zip_family_disambiguation},
    {"slice: describe_buffer adds a checksum and size detect_buffer omits",
     test_slice_describe_buffer_adds_checksum_and_size},
    {"slice: copy_extension handles double extensions and dotfiles",
     test_slice_copy_extension},

    {"boundary: empty and absurdly short buffers never crash",
     test_boundary_empty_and_short_buffers},
    {"boundary: null pointers and a stale struct_size are rejected",
     test_boundary_null_and_bad_struct_size},
    {"boundary: a nonexistent or empty path is a defined error",
     test_boundary_nonexistent_path},
    {"boundary: every registry query is total over a bogus enum value",
     test_boundary_out_of_range_enum_values_are_total},
    {"boundary: an extension hint never substitutes for detection",
     test_boundary_extension_hint_is_advisory_only}};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;
    return uapp_run_test_cases("uapp file detection C ABI tests", k_cases,
                               sizeof k_cases / sizeof k_cases[0], filter);
}
