// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_file.cpp — the C99 face of file-engine/detection.
//
// The house rules from capi_notes.cpp apply unchanged: validate every
// pointer, wrap every body in UAPP_CAPI_GUARD_*, write out-parameters only on
// success, return a status rather than a sentinel.
//
// ---------------------------------------------------------------------------
// THIS IS THE ONLY SURFACE IN THE ABI WITH NO HANDLE AND NO MAGIC COOKIE
// ---------------------------------------------------------------------------
// There is nothing to validate a cookie ON. file-engine/detection holds no
// state: `detect_buffer` and `describe_buffer` are pure functions over bytes,
// and the registry tables they consult are const. So this translation unit
// defines no struct, allocates nothing that outlives a call (with the single
// exception of uapp_file_copy_extension's rule-O string), and has no
// use-after-close failure mode to defend against.
//
// What it does instead is CONVERT: a C-enum integer to a C++ enum and back,
// a `const void*`/`size_t` pair to a ByteSpan, a possibly-NULL `const char*`
// to a std::string_view, and two C++ result types into one flat C struct.
// Every one of those conversions is where a bug at this boundary would live,
// so each is done once in a helper below rather than inline at four call
// sites.
//
// ---------------------------------------------------------------------------
// WHERE THE STRINGS IN uapp_file_info_t POINT, AND WHY IT IS NOT THE File
// ---------------------------------------------------------------------------
// `uapp_file_info_t::mime_type` is the one pointer this struct hands out, and
// it comes from `format_mime_type(format)` — a function returning STATIC
// STORAGE — and NOT from `File::mime_type`, which is a std::string member of
// a value that dies when this function returns.
//
// The two carry identical bytes: `finish_file()` in detector_path.cpp
// assigns `out.mime_type = format_mime_type(detection.format)`. Taking the
// static one is therefore not a different answer, it is the same answer with
// a lifetime the ABI can promise. Taking `file.mime_type.c_str()` would
// compile, pass a quick test, and dangle — which is exactly the class of bug
// uapp_capi.h's rule B exists to make impossible to write by accident.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_file.h"
#include "uapp/file_engine/byte_span.h"
#include "uapp/file_engine/capabilities.h"
#include "uapp/file_engine/checksum.h"
#include "uapp/file_engine/detector.h"
#include "uapp/file_engine/file.h"
#include "uapp/file_engine/format.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::capi::dup_string;
using uapp::capi::to_c_status;
using uapp::files::ByteSpan;
using uapp::files::Capability;
using uapp::files::CapabilitySet;
using uapp::files::Checksum;
using uapp::files::ChecksumAlgorithm;
using uapp::files::DetectionConfidence;
using uapp::files::DetectionHints;
using uapp::files::DetectionMethod;
using uapp::files::DetectionResult;
using uapp::files::File;
using uapp::files::FileFormat;
using uapp::files::FileId;
using uapp::files::FileLocation;
using uapp::files::FormatCategory;

// ---------------------------------------------------------------------------
// The C enums and the C++ enums must agree, forever.
//
// Same mechanism, and same reason, as capi_common.cpp's status asserts and
// capi_search.cpp's source-kind asserts: FileFormat values are persisted in
// the `file.format` column and the capability bits are persisted as a
// bitmask, so a renumbering in file-engine must be a COMPILE ERROR here
// rather than an Android client that reads a stored 7 and renders "xlsx".
// ---------------------------------------------------------------------------

#define UAPP_CAPI_ASSERT_ENUM(cpp_value, c_value)                                   \
    static_assert(static_cast<std::int32_t>(cpp_value) ==                            \
                      static_cast<std::int32_t>(c_value),                            \
                  "uapp_file.h drifted from file-engine for " #cpp_value)

static_assert(sizeof(uapp_file_format_t) == 4u, "uapp_file_format_t must be 32 bits");
static_assert(sizeof(uapp_format_category_t) == 4u, "uapp_format_category_t must be 32 bits");
static_assert(sizeof(uapp_detection_confidence_t) == 4u,
              "uapp_detection_confidence_t must be 32 bits");
static_assert(sizeof(uapp_detection_method_t) == 4u, "uapp_detection_method_t must be 32 bits");
static_assert(sizeof(uapp_checksum_algorithm_t) == 4u,
              "uapp_checksum_algorithm_t must be 32 bits");
static_assert(sizeof(uapp_file_location_kind_t) == 4u,
              "uapp_file_location_kind_t must be 32 bits");
static_assert(sizeof(uapp_file_capability_t) == 4u, "uapp_file_capability_t must be 32 bits");

// Formats: all 41 of them, implemented and declared alike. The declared ones
// are asserted too — they are persisted values a future build could write,
// so drift in one of those is just as damaging and strictly harder to spot.
UAPP_CAPI_ASSERT_ENUM(FileFormat::Unknown, UAPP_FILE_FORMAT_UNKNOWN);
UAPP_CAPI_ASSERT_ENUM(FileFormat::PlainText, UAPP_FILE_FORMAT_PLAIN_TEXT);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Pdf, UAPP_FILE_FORMAT_PDF);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Png, UAPP_FILE_FORMAT_PNG);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Jpeg, UAPP_FILE_FORMAT_JPEG);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Gif, UAPP_FILE_FORMAT_GIF);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Zip, UAPP_FILE_FORMAT_ZIP);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Docx, UAPP_FILE_FORMAT_DOCX);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Xlsx, UAPP_FILE_FORMAT_XLSX);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Pptx, UAPP_FILE_FORMAT_PPTX);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Odt, UAPP_FILE_FORMAT_ODT);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Markdown, UAPP_FILE_FORMAT_MARKDOWN);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Rtf, UAPP_FILE_FORMAT_RTF);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Doc, UAPP_FILE_FORMAT_DOC);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Html, UAPP_FILE_FORMAT_HTML);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Xml, UAPP_FILE_FORMAT_XML);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Json, UAPP_FILE_FORMAT_JSON);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Csv, UAPP_FILE_FORMAT_CSV);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Xls, UAPP_FILE_FORMAT_XLS);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Ods, UAPP_FILE_FORMAT_ODS);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Ppt, UAPP_FILE_FORMAT_PPT);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Odp, UAPP_FILE_FORMAT_ODP);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Webp, UAPP_FILE_FORMAT_WEBP);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Tiff, UAPP_FILE_FORMAT_TIFF);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Bmp, UAPP_FILE_FORMAT_BMP);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Svg, UAPP_FILE_FORMAT_SVG);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Heic, UAPP_FILE_FORMAT_HEIC);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Avif, UAPP_FILE_FORMAT_AVIF);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Epub, UAPP_FILE_FORMAT_EPUB);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Mobi, UAPP_FILE_FORMAT_MOBI);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Fb2, UAPP_FILE_FORMAT_FB2);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Tar, UAPP_FILE_FORMAT_TAR);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Gzip, UAPP_FILE_FORMAT_GZIP);
UAPP_CAPI_ASSERT_ENUM(FileFormat::SevenZ, UAPP_FILE_FORMAT_SEVEN_Z);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Rar, UAPP_FILE_FORMAT_RAR);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Mp3, UAPP_FILE_FORMAT_MP3);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Wav, UAPP_FILE_FORMAT_WAV);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Flac, UAPP_FILE_FORMAT_FLAC);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Aac, UAPP_FILE_FORMAT_AAC);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Ogg, UAPP_FILE_FORMAT_OGG);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Mp4, UAPP_FILE_FORMAT_MP4);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Mov, UAPP_FILE_FORMAT_MOV);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Mkv, UAPP_FILE_FORMAT_MKV);
UAPP_CAPI_ASSERT_ENUM(FileFormat::Avi, UAPP_FILE_FORMAT_AVI);
UAPP_CAPI_ASSERT_ENUM(FileFormat::WebM, UAPP_FILE_FORMAT_WEBM);
UAPP_CAPI_ASSERT_ENUM(FileFormat::SourceCode, UAPP_FILE_FORMAT_SOURCE_CODE);

UAPP_CAPI_ASSERT_ENUM(FormatCategory::Unknown, UAPP_FORMAT_CATEGORY_UNKNOWN);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Document, UAPP_FORMAT_CATEGORY_DOCUMENT);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Spreadsheet, UAPP_FORMAT_CATEGORY_SPREADSHEET);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Presentation, UAPP_FORMAT_CATEGORY_PRESENTATION);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Pdf, UAPP_FORMAT_CATEGORY_PDF);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Image, UAPP_FORMAT_CATEGORY_IMAGE);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Ebook, UAPP_FORMAT_CATEGORY_EBOOK);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Archive, UAPP_FORMAT_CATEGORY_ARCHIVE);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Audio, UAPP_FORMAT_CATEGORY_AUDIO);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Video, UAPP_FORMAT_CATEGORY_VIDEO);
UAPP_CAPI_ASSERT_ENUM(FormatCategory::Code, UAPP_FORMAT_CATEGORY_CODE);

UAPP_CAPI_ASSERT_ENUM(DetectionConfidence::None, UAPP_DETECTION_CONFIDENCE_NONE);
UAPP_CAPI_ASSERT_ENUM(DetectionConfidence::ExtensionOnly,
                      UAPP_DETECTION_CONFIDENCE_EXTENSION_ONLY);
UAPP_CAPI_ASSERT_ENUM(DetectionConfidence::Heuristic, UAPP_DETECTION_CONFIDENCE_HEURISTIC);
UAPP_CAPI_ASSERT_ENUM(DetectionConfidence::Magic, UAPP_DETECTION_CONFIDENCE_MAGIC);
UAPP_CAPI_ASSERT_ENUM(DetectionConfidence::MagicAndContainer,
                      UAPP_DETECTION_CONFIDENCE_MAGIC_AND_CONTAINER);

UAPP_CAPI_ASSERT_ENUM(DetectionMethod::NoMatch, UAPP_DETECTION_METHOD_NO_MATCH);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::MagicSignature, UAPP_DETECTION_METHOD_MAGIC_SIGNATURE);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::MagicSignatureAtOffset,
                      UAPP_DETECTION_METHOD_MAGIC_SIGNATURE_AT_OFFSET);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::TextHeuristic, UAPP_DETECTION_METHOD_TEXT_HEURISTIC);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::ZipCentralDirectory,
                      UAPP_DETECTION_METHOD_ZIP_CENTRAL_DIRECTORY);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::ZipLocalHeaders,
                      UAPP_DETECTION_METHOD_ZIP_LOCAL_HEADERS);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::ZipMimetypeMember,
                      UAPP_DETECTION_METHOD_ZIP_MIMETYPE_MEMBER);
UAPP_CAPI_ASSERT_ENUM(DetectionMethod::ExtensionHintOnly,
                      UAPP_DETECTION_METHOD_EXTENSION_HINT_ONLY);

UAPP_CAPI_ASSERT_ENUM(ChecksumAlgorithm::None, UAPP_CHECKSUM_ALGORITHM_NONE);
UAPP_CAPI_ASSERT_ENUM(ChecksumAlgorithm::Fnv1a64, UAPP_CHECKSUM_ALGORITHM_FNV1A64);

UAPP_CAPI_ASSERT_ENUM(FileLocation::Kind::Unknown, UAPP_FILE_LOCATION_UNKNOWN);
UAPP_CAPI_ASSERT_ENUM(FileLocation::Kind::LocalPath, UAPP_FILE_LOCATION_LOCAL_PATH);
UAPP_CAPI_ASSERT_ENUM(FileLocation::Kind::MemoryBuffer, UAPP_FILE_LOCATION_MEMORY_BUFFER);
UAPP_CAPI_ASSERT_ENUM(FileLocation::Kind::PlatformUri, UAPP_FILE_LOCATION_PLATFORM_URI);

UAPP_CAPI_ASSERT_ENUM(Capability::View, UAPP_FILE_CAPABILITY_VIEW);
UAPP_CAPI_ASSERT_ENUM(Capability::Edit, UAPP_FILE_CAPABILITY_EDIT);
UAPP_CAPI_ASSERT_ENUM(Capability::Convert, UAPP_FILE_CAPABILITY_CONVERT);
UAPP_CAPI_ASSERT_ENUM(Capability::Export, UAPP_FILE_CAPABILITY_EXPORT);
UAPP_CAPI_ASSERT_ENUM(Capability::Ocr, UAPP_FILE_CAPABILITY_OCR);
UAPP_CAPI_ASSERT_ENUM(Capability::Annotate, UAPP_FILE_CAPABILITY_ANNOTATE);
UAPP_CAPI_ASSERT_ENUM(Capability::Compress, UAPP_FILE_CAPABILITY_COMPRESS);
UAPP_CAPI_ASSERT_ENUM(Capability::Sign, UAPP_FILE_CAPABILITY_SIGN);
UAPP_CAPI_ASSERT_ENUM(Capability::Encrypt, UAPP_FILE_CAPABILITY_ENCRYPT);
UAPP_CAPI_ASSERT_ENUM(Capability::Extract, UAPP_FILE_CAPABILITY_EXTRACT);

#undef UAPP_CAPI_ASSERT_ENUM

// The public macro must equal the C++ array bound, or a caller sizing a
// buffer from the header writes into a struct the implementation thinks is
// bigger. Checked here because this is the only place both are visible.
static_assert(UAPP_MAX_CHECKSUM_BYTES == uapp::files::kMaxChecksumBytes,
              "UAPP_MAX_CHECKSUM_BYTES must equal uapp::files::kMaxChecksumBytes");
static_assert(uapp::files::kCapabilityCount == 10u,
              "spec section 8 names ten capabilities; uapp_file.h declares ten bits");

// --- conversions -----------------------------------------------------------

/// A C enum integer to FileFormat. NOT range-checked: the C++ registry
/// functions are all total (they answer "unknown" for an integer that is not
/// a format), so clamping here would replace an honest "unknown" with a
/// wrong-but-plausible "plain text". The cast is the whole conversion.
[[nodiscard]] FileFormat to_format(uapp_file_format_t format) noexcept {
    return static_cast<FileFormat>(static_cast<std::int32_t>(format));
}

[[nodiscard]] uapp_file_format_t from_format(FileFormat format) noexcept {
    return static_cast<uapp_file_format_t>(static_cast<std::int32_t>(format));
}

/// A possibly-NULL C string to a view. NULL becomes an EMPTY view rather
/// than a view over nothing-in-particular: DetectionHints::file_name is
/// documented as optional, and "no name" is spelled "empty" on the C++ side.
[[nodiscard]] std::string_view to_view(const char* s) noexcept {
    return s == nullptr ? std::string_view() : std::string_view(s);
}

/// How many registry formats have a sniffer in this build.
///
/// Counted rather than read from a constant because file-engine has no
/// equivalent of blocks::kFirstUnimplementedBlockType — its implemented set
/// is not a prefix of the enum, it is 1..10 with Unknown at 0. Counting over
/// the public predicate is therefore the only way to get the number without
/// this layer keeping its own list, which would be the one thing guaranteed
/// to drift.
[[nodiscard]] std::size_t implemented_format_count() noexcept {
    std::size_t implemented = 0;
    const std::size_t total = uapp::files::format_count();
    for (std::size_t i = 0; i < total; ++i) {
        if (uapp::files::format_detection_is_implemented(uapp::files::format_at(i))) {
            ++implemented;
        }
    }
    return implemented;
}

/// True iff `bits` is exactly one of the ten capability bits. Used so that
/// uapp_file_capability_name() answers "unknown" for a MASK rather than
/// naming whichever single bit happens to compare equal first.
[[nodiscard]] bool is_single_capability(std::uint32_t bits) noexcept {
    for (std::uint32_t i = 0; i < uapp::files::kCapabilityCount; ++i) {
        if (static_cast<std::uint32_t>(uapp::files::capability_at(i)) == bits) {
            return true;
        }
    }
    return false;
}

// --- filling the out-struct ------------------------------------------------

/// Reset every field to the documented "nothing detected" state.
///
/// Explicit rather than a memset over the whole struct: struct_size may say
/// the caller's object is LARGER than ours (a caller from the future), and
/// zeroing past our sizeof would be writing into fields we do not own. So
/// this writes the members this build knows about and nothing else, which is
/// the same discipline the limits functions follow in the other direction.
void clear_info(uapp_file_info_t* info) noexcept {
    info->format              = UAPP_FILE_FORMAT_UNKNOWN;
    info->container_format    = UAPP_FILE_FORMAT_UNKNOWN;
    info->confidence          = UAPP_DETECTION_CONFIDENCE_NONE;
    info->method              = UAPP_DETECTION_METHOD_NO_MATCH;
    info->extension_hint      = UAPP_FILE_FORMAT_UNKNOWN;
    info->extension_conflicts = UAPP_FALSE;
    info->container_truncated = UAPP_FALSE;
    info->unrecognised_ooxml  = UAPP_FALSE;
    info->magic_offset        = 0;
    // Never NULL, even in the cleared state: the header promises a readable
    // media type unconditionally, and "application/octet-stream" is the
    // correct answer for bytes we cannot identify.
    info->mime_type                 = uapp::files::format_mime_type(FileFormat::Unknown);
    info->category                  = UAPP_FORMAT_CATEGORY_UNKNOWN;
    info->capabilities              = 0u;
    info->extension_present         = UAPP_FALSE;
    info->extension_matches_content = UAPP_FALSE;
    info->location_kind             = UAPP_FILE_LOCATION_UNKNOWN;
    info->size_bytes                = 0;
    info->checksum_algorithm        = UAPP_CHECKSUM_ALGORITHM_NONE;
    info->checksum_length           = 0;
    std::memset(info->checksum_bytes, 0, sizeof info->checksum_bytes);
}

/// The verdict half, plus the three registry-derived File fields that are a
/// pure function of the detected format.
///
/// mime_type/category/capabilities are computed from `result.format` here by
/// the same three calls `finish_file()` makes in detector_path.cpp. That is
/// not a second opinion, it is the same function applied to the same input —
/// and the describe_* routes below overwrite `capabilities` from the File
/// they were handed anyway, so the authoritative value wins on the route
/// that has one.
void fill_from_detection(const DetectionResult& result, uapp_file_info_t* info) noexcept {
    info->format           = from_format(result.format);
    info->container_format = from_format(result.container_format);
    info->confidence =
        static_cast<uapp_detection_confidence_t>(static_cast<std::int32_t>(result.confidence));
    info->method =
        static_cast<uapp_detection_method_t>(static_cast<std::int32_t>(result.method));
    info->extension_hint      = from_format(result.extension_hint);
    info->extension_conflicts = result.extension_conflicts ? UAPP_TRUE : UAPP_FALSE;
    info->container_truncated = result.container_truncated ? UAPP_TRUE : UAPP_FALSE;
    info->unrecognised_ooxml  = result.unrecognised_ooxml ? UAPP_TRUE : UAPP_FALSE;
    info->magic_offset        = result.magic_offset;
    info->mime_type           = uapp::files::format_mime_type(result.format);
    info->category = static_cast<uapp_format_category_t>(
        static_cast<std::int32_t>(uapp::files::format_category(result.format)));
    info->capabilities = uapp::files::format_capabilities(result.format).bits();
}

/// The two extension-agreement flags, for the detect_* routes, which get a
/// DetectionResult and therefore no FileMetadata.
///
/// This is the one place this file reproduces a two-line expression from
/// `fill_metadata()` rather than reading it off a value. The alternative was
/// to leave both flags UAPP_FALSE on the detect routes and document it,
/// which would mean two fields whose meaning depends on which function the
/// caller happened to call — the exact ambiguity the single flat struct
/// exists to avoid. `extension_of` is public API, so the derivation is a
/// call plus a boolean AND, and the describe_* routes assert agreement with
/// the File's own metadata in tests/capi/file_capi_test.c.
void fill_extension_flags(std::string_view file_name, const DetectionResult& result,
                          uapp_file_info_t* info) {
    const bool present = !uapp::files::extension_of(file_name).empty();
    info->extension_present = present ? UAPP_TRUE : UAPP_FALSE;
    info->extension_matches_content =
        (present && !result.extension_conflicts) ? UAPP_TRUE : UAPP_FALSE;
}

/// The File half: size, checksum, location and the authoritative capability
/// set and metadata flags.
void fill_from_file(const File& file, uapp_file_info_t* info) noexcept {
    info->size_bytes   = file.size;
    info->capabilities = file.capabilities.bits();
    info->location_kind =
        static_cast<uapp_file_location_kind_t>(static_cast<std::int32_t>(file.location.kind));
    info->extension_present = file.metadata.extension_present ? UAPP_TRUE : UAPP_FALSE;
    info->extension_matches_content =
        file.metadata.extension_matches_content ? UAPP_TRUE : UAPP_FALSE;

    const Checksum& checksum = file.checksum;
    info->checksum_algorithm =
        static_cast<uapp_checksum_algorithm_t>(static_cast<std::int32_t>(checksum.algorithm));
    // `length` is a uint8_t in the C++ struct and the array is 32 bytes, so
    // this cannot exceed the bound — but clamp anyway rather than trust it:
    // the memcpy below is the one place in this file that writes a
    // caller-owned array by length, and "it cannot be wrong" is not a bounds
    // check.
    std::size_t length = checksum.length;
    if (length > sizeof info->checksum_bytes) {
        length = sizeof info->checksum_bytes;
    }
    info->checksum_length = static_cast<std::uint32_t>(length);
    if (length > 0u) {
        std::memcpy(info->checksum_bytes, checksum.bytes.data(), length);
    }
}

/// Shared prologue: the out-pointer and its struct_size.
///
/// A struct_size SMALLER than ours is rejected — honouring it would mean
/// writing past the end of the caller's object. A LARGER one (a caller
/// compiled against a future header) is accepted, and the tail this build
/// does not know about is left exactly as the caller left it.
[[nodiscard]] uapp_status_t check_out_info(uapp_file_info_t* out_info) noexcept {
    if (out_info == nullptr) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    if (out_info->struct_size < sizeof(uapp_file_info_t)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    return UAPP_STATUS_OK;
}

/// `bytes`/`size` to a ByteSpan, rejecting the one combination that is a
/// caller bug rather than an empty input.
///
/// NULL with size 0 is NOT an error: it is an empty file, reported as
/// UAPP_STATUS_FILE_EMPTY by the engine like any other empty input. NULL
/// with a non-zero size is a lie about readable memory, and ByteSpan would
/// silently treat it as empty — which would turn "I passed the wrong
/// pointer" into "your file is empty". Those must not be the same answer.
[[nodiscard]] uapp_status_t to_span(const void* bytes, std::size_t size,
                                    ByteSpan& out) noexcept {
    if (bytes == nullptr && size != 0u) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    out = ByteSpan(static_cast<const std::uint8_t*>(bytes), size);
    return UAPP_STATUS_OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

// --- registry lookups ------------------------------------------------------

size_t uapp_file_format_count(void) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_count();
    UAPP_CAPI_GUARD_END_FALLBACK(0u)
}

uapp_file_format_t uapp_file_format_at(size_t index) {
    UAPP_CAPI_GUARD_BEGIN
        return from_format(uapp::files::format_at(index));
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FILE_FORMAT_UNKNOWN)
}

const char* uapp_file_format_name(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_name(to_format(format));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

uapp_format_category_t uapp_file_format_category(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return static_cast<uapp_format_category_t>(
            static_cast<std::int32_t>(uapp::files::format_category(to_format(format))));
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FORMAT_CATEGORY_UNKNOWN)
}

const char* uapp_format_category_name(uapp_format_category_t category) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_category_name(
            static_cast<FormatCategory>(static_cast<std::int32_t>(category)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

const char* uapp_file_format_mime_type(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_mime_type(to_format(format));
    UAPP_CAPI_GUARD_END_FALLBACK("application/octet-stream")
}

const char* uapp_file_format_primary_extension(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_primary_extension(to_format(format));
    UAPP_CAPI_GUARD_END_FALLBACK("")
}

uapp_bool_t uapp_file_format_detection_is_implemented(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_detection_is_implemented(to_format(format)) ? UAPP_TRUE
                                                                               : UAPP_FALSE;
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FALSE)
}

const char* uapp_file_format_detection_not_implemented_reason(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_detection_not_implemented_reason(to_format(format));
    UAPP_CAPI_GUARD_END_FALLBACK(NULL)
}

uapp_file_format_t uapp_file_format_from_extension(const char* extension) {
    UAPP_CAPI_GUARD_BEGIN
        return from_format(uapp::files::format_from_extension(to_view(extension)));
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FILE_FORMAT_UNKNOWN)
}

// --- capabilities ----------------------------------------------------------

uint32_t uapp_file_capability_count(void) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::kCapabilityCount;
    UAPP_CAPI_GUARD_END_FALLBACK(0u)
}

uapp_file_capability_t uapp_file_capability_at(uint32_t index) {
    UAPP_CAPI_GUARD_BEGIN
        // capability_at() clamps an out-of-range index to View, which the
        // public header documents for both layers. Passing it through rather
        // than second-guessing it keeps one answer in the product.
        return static_cast<uapp_file_capability_t>(
            static_cast<std::int32_t>(uapp::files::capability_at(index)));
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FILE_CAPABILITY_VIEW)
}

const char* uapp_file_capability_name(uapp_file_capability_t capability) {
    UAPP_CAPI_GUARD_BEGIN
        const std::int32_t value = static_cast<std::int32_t>(capability);
        if (value <= 0 || !is_single_capability(static_cast<std::uint32_t>(value))) {
            return "unknown";
        }
        return uapp::files::capability_name(
            static_cast<Capability>(static_cast<std::uint32_t>(value)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

uapp_bool_t uapp_file_capabilities_have(uint32_t capabilities,
                                        uapp_file_capability_t capability) {
    UAPP_CAPI_GUARD_BEGIN
        const std::int32_t value = static_cast<std::int32_t>(capability);
        if (value <= 0 || !is_single_capability(static_cast<std::uint32_t>(value))) {
            // Not exactly one capability. Answering UAPP_FALSE rather than
            // doing the bit test anyway means a mask of two capabilities is
            // never quietly reported as "present".
            return UAPP_FALSE;
        }
        return CapabilitySet(capabilities)
                       .has(static_cast<Capability>(static_cast<std::uint32_t>(value)))
                   ? UAPP_TRUE
                   : UAPP_FALSE;
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FALSE)
}

uint32_t uapp_file_format_capabilities(uapp_file_format_t format) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::format_capabilities(to_format(format)).bits();
    UAPP_CAPI_GUARD_END_FALLBACK(0u)
}

// --- verdict metadata names ------------------------------------------------

const char* uapp_detection_confidence_name(uapp_detection_confidence_t confidence) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::detection_confidence_name(
            static_cast<DetectionConfidence>(static_cast<std::int32_t>(confidence)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

const char* uapp_detection_method_name(uapp_detection_method_t method) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::detection_method_name(
            static_cast<DetectionMethod>(static_cast<std::int32_t>(method)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

const char* uapp_checksum_algorithm_name(uapp_checksum_algorithm_t algorithm) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::checksum_algorithm_name(
            static_cast<ChecksumAlgorithm>(static_cast<std::int32_t>(algorithm)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

const char* uapp_file_location_kind_name(uapp_file_location_kind_t kind) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::files::file_location_kind_name(
            static_cast<FileLocation::Kind>(static_cast<std::int32_t>(kind)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

// --- limits ----------------------------------------------------------------

uapp_status_t uapp_file_get_limits(uapp_file_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (out_limits->struct_size < sizeof(uapp_file_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->head_sample_bytes        = uapp::files::kHeadSampleBytes;
        out_limits->tail_sample_bytes        = uapp::files::kTailSampleBytes;
        out_limits->text_probe_bytes         = uapp::files::kTextProbeBytes;
        out_limits->max_checksum_bytes       = uapp::files::kMaxChecksumBytes;
        out_limits->format_count             = uapp::files::format_count();
        out_limits->implemented_format_count = implemented_format_count();
        out_limits->capability_count         = uapp::files::kCapabilityCount;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- the verdict -----------------------------------------------------------

uapp_status_t uapp_file_info_init(uapp_file_info_t* out_info) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_info == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_info->struct_size = static_cast<std::uint32_t>(sizeof(uapp_file_info_t));
        clear_info(out_info);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_file_detect_buffer(const void* bytes, size_t size,
                                      const char* file_name_hint,
                                      uapp_file_info_t* out_info) {
    UAPP_CAPI_GUARD_BEGIN
        const uapp_status_t checked = check_out_info(out_info);
        if (checked != UAPP_STATUS_OK) {
            return checked;
        }
        ByteSpan span;
        const uapp_status_t spanned = to_span(bytes, size, span);
        if (spanned != UAPP_STATUS_OK) {
            return spanned;
        }

        DetectionHints hints;
        hints.file_name = to_view(file_name_hint);
        // declared_mime_type is left empty: the C++ field is accepted and
        // IGNORED, so uapp_file.h does not offer a parameter the core throws
        // away. See the NOT IMPLEMENTED note there.

        DetectionResult result;
        const Status status = uapp::files::detect_buffer(span, hints, result);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        clear_info(out_info);
        fill_from_detection(result, out_info);
        fill_extension_flags(to_view(file_name_hint), result, out_info);
        // Bytes with no durable location — which is exactly what
        // FileLocation::Kind::MemoryBuffer means, and what describe_buffer()
        // records for the same input.
        out_info->location_kind = UAPP_FILE_LOCATION_MEMORY_BUFFER;
        out_info->size_bytes    = size;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_file_detect_path(const char* path, uapp_file_info_t* out_info) {
    UAPP_CAPI_GUARD_BEGIN
        const uapp_status_t checked = check_out_info(out_info);
        if (checked != UAPP_STATUS_OK) {
            return checked;
        }
        if (path == nullptr) {
            // Distinct from an EMPTY path, which is the core's
            // FILE_PATH_INVALID. A NULL pointer is a caller bug at the
            // boundary and never reaches the engine.
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        DetectionResult result;
        const Status status = uapp::files::detect_path(std::string_view(path), result);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        clear_info(out_info);
        fill_from_detection(result, out_info);
        // detect_path takes the extension hint from the path's own final
        // component, so the flags are derived from the same string.
        fill_extension_flags(
            std::string(uapp::files::file_name_of_path(std::string_view(path))).c_str(), result,
            out_info);
        out_info->location_kind = UAPP_FILE_LOCATION_LOCAL_PATH;
        // size_bytes stays 0 and the checksum stays NONE: this route reads
        // bounded windows and never learns the total, which uapp_file.h
        // documents on both fields.
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_file_describe_buffer(const void* bytes, size_t size,
                                        const char* file_name_hint, int64_t created_at_ms,
                                        int64_t modified_at_ms, uapp_file_info_t* out_info) {
    UAPP_CAPI_GUARD_BEGIN
        const uapp_status_t checked = check_out_info(out_info);
        if (checked != UAPP_STATUS_OK) {
            return checked;
        }
        ByteSpan span;
        const uapp_status_t spanned = to_span(bytes, size, span);
        if (spanned != UAPP_STATUS_OK) {
            return spanned;
        }

        DetectionHints hints;
        hints.file_name = to_view(file_name_hint);

        File         file;
        const Status described = uapp::files::describe_buffer(
            span, hints, FileId(), created_at_ms, modified_at_ms, file);
        if (!uapp::is_ok(described)) {
            return to_c_status(described);
        }

        // The second call, over the same bytes. describe_buffer() runs
        // detection internally and keeps none of the verdict (no confidence,
        // no method, no container flags), so the only ways to report them
        // are this or reconstructing them here — and reconstructing a
        // verdict at the ABI boundary is exactly what this layer must not
        // do. Detection is pure, allocation-free and bounded by the sample
        // windows; the expensive part, hashing every byte, happened once
        // above. See the struct comment in uapp_file.h.
        DetectionResult result;
        const Status detected = uapp::files::detect_buffer(span, hints, result);
        if (!uapp::is_ok(detected)) {
            return to_c_status(detected);
        }

        clear_info(out_info);
        fill_from_detection(result, out_info);
        fill_from_file(file, out_info);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_file_describe_path(const char* path, uapp_file_info_t* out_info) {
    UAPP_CAPI_GUARD_BEGIN
        const uapp_status_t checked = check_out_info(out_info);
        if (checked != UAPP_STATUS_OK) {
            return checked;
        }
        if (path == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        File         file;
        const Status described =
            uapp::files::describe_path(std::string_view(path), FileId(), file);
        if (!uapp::is_ok(described)) {
            return to_c_status(described);
        }

        DetectionResult result;
        const Status detected = uapp::files::detect_path(std::string_view(path), result);
        if (!uapp::is_ok(detected)) {
            return to_c_status(detected);
        }

        clear_info(out_info);
        fill_from_detection(result, out_info);
        fill_from_file(file, out_info);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- the one owned string --------------------------------------------------

uapp_status_t uapp_file_copy_extension(const char* name, char** out_extension) {
    UAPP_CAPI_GUARD_BEGIN
        if (name == nullptr || out_extension == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const std::string extension = uapp::files::extension_of(std::string_view(name));
        char* copy = dup_string(extension.c_str(), extension.size());
        if (copy == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_extension = copy;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

}  // extern "C"
