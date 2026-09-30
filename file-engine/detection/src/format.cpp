// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/format.h"

#include <cstddef>

namespace uapp::files {
namespace {

/// The registry, in declaration order. One array, so `format_count()` and
/// every exhaustiveness test agree with the enum by construction.
constexpr FileFormat kAllFormats[] = {
    FileFormat::Unknown,
    // detection implemented
    FileFormat::PlainText, FileFormat::Pdf,  FileFormat::Png,  FileFormat::Jpeg,
    FileFormat::Gif,       FileFormat::Zip,  FileFormat::Docx, FileFormat::Xlsx,
    FileFormat::Pptx,      FileFormat::Odt,
    // detection not implemented
    FileFormat::Markdown, FileFormat::Rtf,  FileFormat::Doc,  FileFormat::Html,
    FileFormat::Xml,      FileFormat::Json, FileFormat::Csv,  FileFormat::Xls,
    FileFormat::Ods,      FileFormat::Ppt,  FileFormat::Odp,  FileFormat::Webp,
    FileFormat::Tiff,     FileFormat::Bmp,  FileFormat::Svg,  FileFormat::Heic,
    FileFormat::Avif,     FileFormat::Epub, FileFormat::Mobi, FileFormat::Fb2,
    FileFormat::Tar,      FileFormat::Gzip, FileFormat::SevenZ, FileFormat::Rar,
    FileFormat::Mp3,      FileFormat::Wav,  FileFormat::Flac, FileFormat::Aac,
    FileFormat::Ogg,      FileFormat::Mp4,  FileFormat::Mov,  FileFormat::Mkv,
    FileFormat::Avi,      FileFormat::WebM, FileFormat::SourceCode,
};

// --- the NOT-IMPLEMENTED table ---------------------------------------------
// Grouped by the actual engineering reason, not by category. Each reason says
// what is missing, so "why not?" has an answer that survives review.

constexpr const char* kReasonBinaryOffice =
    "NOT IMPLEMENTED: the OLE2/CFB compound-file header is shared by DOC, XLS, PPT and "
    "several unrelated formats, so sniffing it identifies the *container*, not the "
    "document type; telling them apart needs the CFB directory walked, which is a real "
    "parser. file-format-support.md §2 also marks all three server-only (no viable "
    "permissively-licensed C/C++ parser exists), so a client-side answer would be "
    "detection with nothing behind it.";

constexpr const char* kReasonTextDialect =
    "NOT IMPLEMENTED: has no magic number and is a dialect of plain text. Distinguishing "
    "it from PlainText requires content analysis (grammar sniffing), which is a parser "
    "decision with a real false-positive rate, not a signature match. These currently "
    "detect as PlainText, which is true rather than wrong.";

constexpr const char* kReasonZipSubtype =
    "NOT IMPLEMENTED: a ZIP container like DOCX/XLSX/PPTX/ODT. The container introspection "
    "that would classify it already exists (see zip_container.h); only the member-path "
    "signature for this subtype is missing. Detects as Zip today.";

constexpr const char* kReasonIsoBmff =
    "NOT IMPLEMENTED: an ISO base media file (`ftyp` box). Identifying the specific format "
    "means reading the major brand and compatible-brands list, and the brand space is "
    "shared across MP4/MOV/HEIC/AVIF — a partial implementation would mislabel one as "
    "another. Worth doing as one ISO-BMFF sniffer, not four half-sniffers.";

constexpr const char* kReasonMediaDeferred =
    "NOT IMPLEMENTED: audio/video support is deferred pending PQ-5 and the FFmpeg decision "
    "(file-format-support.md §7.6). Detection without playback or metadata behind it would "
    "create a capability claim we cannot honour.";

constexpr const char* kReasonNoEngine =
    "NOT IMPLEMENTED: the signature is trivial, but no engine consumes this format yet, and "
    "an unconsumed detection result is a claim with nothing behind it. Add the sniffer in "
    "the same change as the decoder.";

constexpr const char* kReasonOutOfScope =
    "NOT IMPLEMENTED — PERMANENTLY OUT OF SCOPE: file-format-support.md marks this format "
    "licence-blocked or unsupported for v1.0 and beyond (RAR: the unRAR licence; see §7.7). "
    "Detection exists in the registry only so the product can name the format when it "
    "refuses it.";

constexpr const char* kReasonSourceCode =
    "NOT IMPLEMENTED: source code is plain text. Which *language* a text file holds is a "
    "highlighter concern decided by extension plus heuristics, not a file-type question, "
    "and the twelve spec §9 languages share no signature. Source files detect as PlainText.";

const char* unimplemented_reason(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::Doc:
        case FileFormat::Xls:
        case FileFormat::Ppt:
            return kReasonBinaryOffice;

        case FileFormat::Markdown:
        case FileFormat::Html:
        case FileFormat::Xml:
        case FileFormat::Json:
        case FileFormat::Csv:
        case FileFormat::Svg:
        case FileFormat::Fb2:
        case FileFormat::Rtf:
            return kReasonTextDialect;

        case FileFormat::Ods:
        case FileFormat::Odp:
        case FileFormat::Epub:
            return kReasonZipSubtype;

        case FileFormat::Heic:
        case FileFormat::Avif:
        case FileFormat::Mp4:
        case FileFormat::Mov:
            return kReasonIsoBmff;

        case FileFormat::Mp3:
        case FileFormat::Wav:
        case FileFormat::Flac:
        case FileFormat::Aac:
        case FileFormat::Ogg:
        case FileFormat::Mkv:
        case FileFormat::Avi:
        case FileFormat::WebM:
            return kReasonMediaDeferred;

        case FileFormat::Webp:
        case FileFormat::Tiff:
        case FileFormat::Bmp:
        case FileFormat::Tar:
        case FileFormat::Gzip:
        case FileFormat::SevenZ:
        case FileFormat::Mobi:
            return kReasonNoEngine;

        case FileFormat::Rar:
            return kReasonOutOfScope;

        case FileFormat::SourceCode:
            return kReasonSourceCode;

        // Implemented, or the "no match" sentinel: no reason to give.
        case FileFormat::Unknown:
        case FileFormat::PlainText:
        case FileFormat::Pdf:
        case FileFormat::Png:
        case FileFormat::Jpeg:
        case FileFormat::Gif:
        case FileFormat::Zip:
        case FileFormat::Docx:
        case FileFormat::Xlsx:
        case FileFormat::Pptx:
        case FileFormat::Odt:
            return nullptr;
    }
    return "NOT IMPLEMENTED: unrecognised format identifier.";
}

// --- the capability table --------------------------------------------------
// Transcribed from docs/file-format-support.md by the rules in
// capabilities.h. Each row cites the matrix cells it came from, because the
// value of this table is entirely in its traceability.

CapabilitySet capabilities_for(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::Pdf:
            // §5: VIEW ✔ EDIT ✔ CREATE ✔ CONVERT ✔ OCR ✔ ANNOTATION ✔.
            // ENCRYPT: the "PDF, encrypted" row — QPDF standard security
            // handler R2–R6. COMPRESS: QPDF object streams + linearisation.
            // SIGN: withheld. §5.1 "Digital signature validation is P2/TBD".
            return make_capabilities(Capability::View, Capability::Edit, Capability::Convert,
                                     Capability::Export, Capability::Ocr, Capability::Annotate,
                                     Capability::Compress, Capability::Encrypt,
                                     Capability::Extract);

        case FileFormat::Png:
            // §6 PNG row: all ✔. COMPRESS: lossless re-deflate.
            return make_capabilities(Capability::View, Capability::Edit, Capability::Convert,
                                     Capability::Export, Capability::Ocr, Capability::Annotate,
                                     Capability::Compress);

        case FileFormat::Jpeg:
            // §6 JPEG row: all ✔. COMPRESS withheld — the same row says
            // re-encoding is lossy and must be disclosed, so "optimise this
            // file" is not a safe silent action.
            return make_capabilities(Capability::View, Capability::Edit, Capability::Convert,
                                     Capability::Export, Capability::Ocr, Capability::Annotate);

        case FileFormat::Gif:
            // §6 GIF row: VIEW ✔, EDIT ◐ (single frame), CREATE ✖ -> no
            // EXPORT, CONVERT ✔, OCR ◐, ANNOTATION ✔.
            return make_capabilities(Capability::View, Capability::Edit, Capability::Convert,
                                     Capability::Ocr, Capability::Annotate);

        case FileFormat::Zip:
            // §8 ZIP row: VIEW(browse) ✔, EDIT ◐, CREATE ✔, CONVERT —.
            // ENCRYPT withheld: AES-encrypted ZIP is explicitly read-only.
            return make_capabilities(Capability::View, Capability::Edit, Capability::Export,
                                     Capability::Compress, Capability::Extract);

        case FileFormat::Docx:
            // §2 DOCX row: VIEW ◐, **EDIT P2**, CREATE ◐, CONVERT ◐,
            // ANNOTATION ✖. EDIT is withheld: P2 is post-1.0, so an Edit
            // button here would be a promise this build cannot keep.
            return make_capabilities(Capability::View, Capability::Convert, Capability::Export,
                                     Capability::Extract);

        case FileFormat::Xlsx:
            // §3 XLSX row: VIEW ◐, EDIT ◐, CREATE ◐, CONVERT ◐.
            return make_capabilities(Capability::View, Capability::Edit, Capability::Convert,
                                     Capability::Export, Capability::Extract);

        case FileFormat::Pptx:
            // §4 PPTX row: VIEW ◐ **P2**, EDIT ✖, CREATE ✖, CONVERT ◐ P2.
            // v1.0 ships "text + notes extraction for search only". That is
            // EXTRACT and nothing else. Claiming VIEW here would be the
            // single most misleading row in the table.
            return make_capabilities(Capability::Extract);

        case FileFormat::Odt:
            // §2 ODT row: VIEW ◐, EDIT ✖, CREATE ✖, CONVERT ◐.
            return make_capabilities(Capability::View, Capability::Convert,
                                     Capability::Extract);

        case FileFormat::PlainText:
            // §2 TXT row: VIEW ✔ EDIT ✔ CREATE ✔ CONVERT ✔, ANNOTATION ✖.
            return make_capabilities(Capability::View, Capability::Edit, Capability::Convert,
                                     Capability::Export);

        default:
            // Everything whose detection is not implemented, and Unknown.
            //
            // A format we cannot even identify gets NO capabilities — not a
            // default of VIEW. `default:` rather than an exhaustive list is
            // deliberate here and only here: a newly declared format must
            // start with zero claims until someone writes its matrix-cited
            // row above.
            return CapabilitySet();
    }
}

/// Case-insensitive ASCII compare. The extension came from a filename, which
/// is untrusted, so no locale-sensitive function touches it.
bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') { ca = static_cast<char>(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = static_cast<char>(cb - 'A' + 'a'); }
        if (ca != cb) { return false; }
    }
    return true;
}

struct ExtensionRow final {
    const char* extension;
    FileFormat  format;
};

/// Extension -> claimed format. Advisory only (see format.h). Multiple
/// extensions may claim one format; the reverse mapping lives in
/// format_primary_extension().
constexpr ExtensionRow kExtensionTable[] = {
    {"txt", FileFormat::PlainText},  {"text", FileFormat::PlainText},
    {"log", FileFormat::PlainText},  {"pdf", FileFormat::Pdf},
    {"png", FileFormat::Png},        {"jpg", FileFormat::Jpeg},
    {"jpeg", FileFormat::Jpeg},      {"jpe", FileFormat::Jpeg},
    {"gif", FileFormat::Gif},        {"zip", FileFormat::Zip},
    {"docx", FileFormat::Docx},      {"xlsx", FileFormat::Xlsx},
    {"pptx", FileFormat::Pptx},      {"odt", FileFormat::Odt},
    {"md", FileFormat::Markdown},    {"markdown", FileFormat::Markdown},
    {"rtf", FileFormat::Rtf},        {"doc", FileFormat::Doc},
    {"html", FileFormat::Html},      {"htm", FileFormat::Html},
    {"xml", FileFormat::Xml},        {"json", FileFormat::Json},
    {"csv", FileFormat::Csv},        {"tsv", FileFormat::Csv},
    {"xls", FileFormat::Xls},        {"ods", FileFormat::Ods},
    {"ppt", FileFormat::Ppt},        {"odp", FileFormat::Odp},
    {"webp", FileFormat::Webp},      {"tif", FileFormat::Tiff},
    {"tiff", FileFormat::Tiff},      {"bmp", FileFormat::Bmp},
    {"svg", FileFormat::Svg},        {"heic", FileFormat::Heic},
    {"heif", FileFormat::Heic},      {"avif", FileFormat::Avif},
    {"epub", FileFormat::Epub},      {"mobi", FileFormat::Mobi},
    {"prc", FileFormat::Mobi},       {"fb2", FileFormat::Fb2},
    {"tar", FileFormat::Tar},        {"gz", FileFormat::Gzip},
    {"7z", FileFormat::SevenZ},      {"rar", FileFormat::Rar},
    {"mp3", FileFormat::Mp3},        {"wav", FileFormat::Wav},
    {"flac", FileFormat::Flac},      {"aac", FileFormat::Aac},
    {"ogg", FileFormat::Ogg},        {"oga", FileFormat::Ogg},
    {"mp4", FileFormat::Mp4},        {"m4v", FileFormat::Mp4},
    {"mov", FileFormat::Mov},        {"mkv", FileFormat::Mkv},
    {"avi", FileFormat::Avi},        {"webm", FileFormat::WebM},
    // Spec §9's twelve code languages collapse to one registry entry; see
    // the FileFormat::SourceCode comment in format.h.
    {"c", FileFormat::SourceCode},   {"h", FileFormat::SourceCode},
    {"cc", FileFormat::SourceCode},  {"cpp", FileFormat::SourceCode},
    {"hpp", FileFormat::SourceCode}, {"py", FileFormat::SourceCode},
    {"java", FileFormat::SourceCode},{"js", FileFormat::SourceCode},
    {"ts", FileFormat::SourceCode},  {"rs", FileFormat::SourceCode},
    {"go", FileFormat::SourceCode},  {"css", FileFormat::SourceCode},
    {"sql", FileFormat::SourceCode}, {"sh", FileFormat::SourceCode},
};

}  // namespace

std::size_t format_count() noexcept { return sizeof(kAllFormats) / sizeof(kAllFormats[0]); }

FileFormat format_at(std::size_t index) noexcept {
    if (index >= format_count()) { return FileFormat::Unknown; }
    return kAllFormats[index];
}

const char* format_name(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::Unknown:    return "unknown";
        case FileFormat::PlainText:  return "text";
        case FileFormat::Pdf:        return "pdf";
        case FileFormat::Png:        return "png";
        case FileFormat::Jpeg:       return "jpeg";
        case FileFormat::Gif:        return "gif";
        case FileFormat::Zip:        return "zip";
        case FileFormat::Docx:       return "docx";
        case FileFormat::Xlsx:       return "xlsx";
        case FileFormat::Pptx:       return "pptx";
        case FileFormat::Odt:        return "odt";
        case FileFormat::Markdown:   return "markdown";
        case FileFormat::Rtf:        return "rtf";
        case FileFormat::Doc:        return "doc";
        case FileFormat::Html:       return "html";
        case FileFormat::Xml:        return "xml";
        case FileFormat::Json:       return "json";
        case FileFormat::Csv:        return "csv";
        case FileFormat::Xls:        return "xls";
        case FileFormat::Ods:        return "ods";
        case FileFormat::Ppt:        return "ppt";
        case FileFormat::Odp:        return "odp";
        case FileFormat::Webp:       return "webp";
        case FileFormat::Tiff:       return "tiff";
        case FileFormat::Bmp:        return "bmp";
        case FileFormat::Svg:        return "svg";
        case FileFormat::Heic:       return "heic";
        case FileFormat::Avif:       return "avif";
        case FileFormat::Epub:       return "epub";
        case FileFormat::Mobi:       return "mobi";
        case FileFormat::Fb2:        return "fb2";
        case FileFormat::Tar:        return "tar";
        case FileFormat::Gzip:       return "gzip";
        case FileFormat::SevenZ:     return "7z";
        case FileFormat::Rar:        return "rar";
        case FileFormat::Mp3:        return "mp3";
        case FileFormat::Wav:        return "wav";
        case FileFormat::Flac:       return "flac";
        case FileFormat::Aac:        return "aac";
        case FileFormat::Ogg:        return "ogg";
        case FileFormat::Mp4:        return "mp4";
        case FileFormat::Mov:        return "mov";
        case FileFormat::Mkv:        return "mkv";
        case FileFormat::Avi:        return "avi";
        case FileFormat::WebM:       return "webm";
        case FileFormat::SourceCode: return "source_code";
    }
    return "unknown";
}

FormatCategory format_category(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::PlainText:
        case FileFormat::Markdown:
        case FileFormat::Rtf:
        case FileFormat::Doc:
        case FileFormat::Docx:
        case FileFormat::Odt:
        case FileFormat::Html:
        case FileFormat::Xml:
        case FileFormat::Json:
            return FormatCategory::Document;

        case FileFormat::Csv:
        case FileFormat::Xls:
        case FileFormat::Xlsx:
        case FileFormat::Ods:
            return FormatCategory::Spreadsheet;

        case FileFormat::Ppt:
        case FileFormat::Pptx:
        case FileFormat::Odp:
            return FormatCategory::Presentation;

        case FileFormat::Pdf:
            return FormatCategory::Pdf;

        case FileFormat::Png:
        case FileFormat::Jpeg:
        case FileFormat::Gif:
        case FileFormat::Webp:
        case FileFormat::Tiff:
        case FileFormat::Bmp:
        case FileFormat::Svg:
        case FileFormat::Heic:
        case FileFormat::Avif:
            return FormatCategory::Image;

        case FileFormat::Epub:
        case FileFormat::Mobi:
        case FileFormat::Fb2:
            return FormatCategory::Ebook;

        case FileFormat::Zip:
        case FileFormat::Tar:
        case FileFormat::Gzip:
        case FileFormat::SevenZ:
        case FileFormat::Rar:
            return FormatCategory::Archive;

        case FileFormat::Mp3:
        case FileFormat::Wav:
        case FileFormat::Flac:
        case FileFormat::Aac:
        case FileFormat::Ogg:
            return FormatCategory::Audio;

        case FileFormat::Mp4:
        case FileFormat::Mov:
        case FileFormat::Mkv:
        case FileFormat::Avi:
        case FileFormat::WebM:
            return FormatCategory::Video;

        case FileFormat::SourceCode:
            return FormatCategory::Code;

        case FileFormat::Unknown:
            return FormatCategory::Unknown;
    }
    return FormatCategory::Unknown;
}

const char* format_category_name(FormatCategory category) noexcept {
    switch (category) {
        case FormatCategory::Unknown:      return "unknown";
        case FormatCategory::Document:     return "document";
        case FormatCategory::Spreadsheet:  return "spreadsheet";
        case FormatCategory::Presentation: return "presentation";
        case FormatCategory::Pdf:          return "pdf";
        case FormatCategory::Image:        return "image";
        case FormatCategory::Ebook:        return "ebook";
        case FormatCategory::Archive:      return "archive";
        case FormatCategory::Audio:        return "audio";
        case FormatCategory::Video:        return "video";
        case FormatCategory::Code:         return "code";
    }
    return "unknown";
}

const char* format_mime_type(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::PlainText: return "text/plain; charset=utf-8";
        case FileFormat::Pdf:       return "application/pdf";
        case FileFormat::Png:       return "image/png";
        case FileFormat::Jpeg:      return "image/jpeg";
        case FileFormat::Gif:       return "image/gif";
        case FileFormat::Zip:       return "application/zip";
        case FileFormat::Docx:
            return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
        case FileFormat::Xlsx:
            return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
        case FileFormat::Pptx:
            return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
        case FileFormat::Odt:        return "application/vnd.oasis.opendocument.text";
        case FileFormat::Markdown:   return "text/markdown";
        case FileFormat::Rtf:        return "application/rtf";
        case FileFormat::Doc:        return "application/msword";
        case FileFormat::Html:       return "text/html";
        case FileFormat::Xml:        return "application/xml";
        case FileFormat::Json:       return "application/json";
        case FileFormat::Csv:        return "text/csv";
        case FileFormat::Xls:        return "application/vnd.ms-excel";
        case FileFormat::Ods:        return "application/vnd.oasis.opendocument.spreadsheet";
        case FileFormat::Ppt:        return "application/vnd.ms-powerpoint";
        case FileFormat::Odp:        return "application/vnd.oasis.opendocument.presentation";
        case FileFormat::Webp:       return "image/webp";
        case FileFormat::Tiff:       return "image/tiff";
        case FileFormat::Bmp:        return "image/bmp";
        case FileFormat::Svg:        return "image/svg+xml";
        case FileFormat::Heic:       return "image/heic";
        case FileFormat::Avif:       return "image/avif";
        case FileFormat::Epub:       return "application/epub+zip";
        case FileFormat::Mobi:       return "application/x-mobipocket-ebook";
        case FileFormat::Fb2:        return "application/x-fictionbook+xml";
        case FileFormat::Tar:        return "application/x-tar";
        case FileFormat::Gzip:       return "application/gzip";
        case FileFormat::SevenZ:     return "application/x-7z-compressed";
        case FileFormat::Rar:        return "application/vnd.rar";
        case FileFormat::Mp3:        return "audio/mpeg";
        case FileFormat::Wav:        return "audio/wav";
        case FileFormat::Flac:       return "audio/flac";
        case FileFormat::Aac:        return "audio/aac";
        case FileFormat::Ogg:        return "audio/ogg";
        case FileFormat::Mp4:        return "video/mp4";
        case FileFormat::Mov:        return "video/quicktime";
        case FileFormat::Mkv:        return "video/x-matroska";
        case FileFormat::Avi:        return "video/x-msvideo";
        case FileFormat::WebM:       return "video/webm";
        case FileFormat::SourceCode: return "text/plain; charset=utf-8";
        case FileFormat::Unknown:    break;
    }
    // The honest answer for bytes we cannot identify. RFC 2046 §4.5.1: this
    // is precisely "arbitrary binary data".
    return "application/octet-stream";
}

const char* format_primary_extension(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::Unknown:    return "";
        case FileFormat::PlainText:  return "txt";
        case FileFormat::Jpeg:       return "jpg";
        case FileFormat::Markdown:   return "md";
        case FileFormat::Tiff:       return "tif";
        case FileFormat::Gzip:       return "gz";
        case FileFormat::SevenZ:     return "7z";
        case FileFormat::SourceCode: return "txt";
        default:                     break;
    }
    // For the rest the registry name IS the conventional extension.
    return format_name(format);
}

bool format_detection_is_implemented(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::PlainText:
        case FileFormat::Pdf:
        case FileFormat::Png:
        case FileFormat::Jpeg:
        case FileFormat::Gif:
        case FileFormat::Zip:
        case FileFormat::Docx:
        case FileFormat::Xlsx:
        case FileFormat::Pptx:
        case FileFormat::Odt:
            return true;
        default:
            // Unknown included: "no match" is a detection *outcome*, not a
            // format with a sniffer.
            return false;
    }
}

const char* format_detection_not_implemented_reason(FileFormat format) noexcept {
    if (format_detection_is_implemented(format)) { return nullptr; }
    if (format == FileFormat::Unknown) {
        return "NOT A FORMAT: FileFormat::Unknown is the sentinel for 'detection ran and "
               "nothing matched'. Asking why its detection is unimplemented is a category "
               "error; check DetectionResult::confidence instead.";
    }
    return unimplemented_reason(format);
}

FileFormat format_from_extension(std::string_view extension) noexcept {
    if (!extension.empty() && extension.front() == '.') { extension.remove_prefix(1); }
    if (extension.empty() || extension.size() > 16) { return FileFormat::Unknown; }
    for (const ExtensionRow& row : kExtensionTable) {
        if (iequals_ascii(extension, row.extension)) { return row.format; }
    }
    return FileFormat::Unknown;
}

CapabilitySet format_capabilities(FileFormat format) noexcept { return capabilities_for(format); }

}  // namespace uapp::files
