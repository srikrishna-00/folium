// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/format.h — the format registry (spec section 9).
//
// ---------------------------------------------------------------------------
// THE REGISTRY ENUMERATES ALL OF SPEC SECTION 9, NOT JUST WHAT WE DETECT
// ---------------------------------------------------------------------------
// Spec section 9 names ~50 formats across ten categories and says, in the same
// breath, "Do not claim universal support." A registry that listed only the
// ten formats this run implements would satisfy the second sentence by
// pretending the other forty do not exist — and every caller would then be
// unable to tell "we sniffed it and found nothing" from "we have never taught
// the sniffer about MP3".
//
// So every format in spec section 9 has a permanent enum value here, and the
// registry answers three *different* questions:
//
//   format_detection_is_implemented(f)          -> is there a sniffer for f?
//   format_detection_not_implemented_reason(f)  -> if not, why not?
//   DetectionResult::format == Unknown          -> the sniffer ran, no match.
//
// This mirrors `blocks::block_type_not_implemented_reason()`: the NOT
// IMPLEMENTED marker spec section 5 asks for lives in code, where it cannot
// rot, and is queryable by the UI and the future C ABI instead of being a
// comment somebody has to find.
//
// Enum VALUES ARE APPEND-ONLY. They will be persisted in the `file.format`
// column and carried over the C ABI. Never renumber, never reuse.
//
// ---------------------------------------------------------------------------
// WHAT "IMPLEMENTED" MEANS HERE, PRECISELY
// ---------------------------------------------------------------------------
// It means: *detection* is implemented — this module can answer "is this
// file an X?" from its bytes. It says nothing about being able to open,
// render, edit or convert an X. Those are later phases and are tracked
// separately by `format_capabilities()` below, which is derived from
// docs/file-format-support.md, not from this flag.

#ifndef UAPP_FILE_ENGINE_FORMAT_H
#define UAPP_FILE_ENGINE_FORMAT_H

#include <cstdint>
#include <string_view>

#include "uapp/file_engine/capabilities.h"

namespace uapp::files {

/// Top-level grouping, matching the subheadings of spec section 9.
enum class FormatCategory : std::int32_t {
    Unknown       = 0,
    Document      = 1,
    Spreadsheet   = 2,
    Presentation  = 3,
    Pdf           = 4,
    Image         = 5,
    Ebook         = 6,
    Archive       = 7,
    Audio         = 8,
    Video         = 9,
    Code          = 10,
};

/// Every format spec section 9 names, plus the two non-format sentinels.
/// APPEND-ONLY.
enum class FileFormat : std::int32_t {
    /// The sniffer ran and nothing matched. NOT the same as "we have no
    /// sniffer for this" — see format_detection_is_implemented().
    Unknown = 0,

    // --- detection IMPLEMENTED in this build -------------------------------
    PlainText = 1,   ///< UTF-8 / ASCII text, established by content heuristic
    Pdf       = 2,
    Png       = 3,
    Jpeg      = 4,
    Gif       = 5,
    Zip       = 6,   ///< a ZIP we could not classify further
    Docx      = 7,
    Xlsx      = 8,
    Pptx      = 9,
    Odt       = 10,

    // --- declared, detection NOT IMPLEMENTED -------------------------------
    // Documents
    Markdown = 100,
    Rtf      = 101,
    Doc      = 102,
    Html     = 103,
    Xml      = 104,
    Json     = 105,
    // Spreadsheets
    Csv  = 120,
    Xls  = 121,
    Ods  = 122,
    // Presentations
    Ppt = 140,
    Odp = 141,
    // Images
    Webp = 160,
    Tiff = 161,
    Bmp  = 162,
    Svg  = 163,
    Heic = 164,
    Avif = 165,
    // E-books
    Epub = 180,
    Mobi = 181,
    Fb2  = 182,
    // Archives
    Tar    = 200,
    Gzip   = 201,
    SevenZ = 202,
    Rar    = 203,
    // Audio
    Mp3  = 220,
    Wav  = 221,
    Flac = 222,
    Aac  = 223,
    Ogg  = 224,
    // Video
    Mp4  = 240,
    Mov  = 241,
    Mkv  = 242,
    Avi  = 243,
    WebM = 244,
    // Code — spec section 9 lists twelve languages. They are ONE registry
    // entry, not twelve, because they are all plain text: they are
    // distinguishable only by extension and content heuristics, never by a
    // magic number, and conflating "which language" with "what kind of file"
    // is how a registry ends up with a `.ts` entry that cannot decide between
    // TypeScript and MPEG transport stream.
    SourceCode = 260,
};

/// Iteration support for exhaustive tests and for a UI that wants to list the
/// registry. `format_at(i)` returns Unknown for an out-of-range index.
[[nodiscard]] std::size_t format_count() noexcept;
[[nodiscard]] FileFormat  format_at(std::size_t index) noexcept;

/// Stable lowercase identifier, e.g. "docx". Never null; "unknown" for a
/// bogus integer cast into the enum. This is the string that goes into logs.
[[nodiscard]] const char* format_name(FileFormat format) noexcept;

[[nodiscard]] FormatCategory format_category(FileFormat format) noexcept;
[[nodiscard]] const char*    format_category_name(FormatCategory category) noexcept;

/// The canonical IANA media type. Never null. Unknown maps to
/// "application/octet-stream", which is the correct answer for "bytes we
/// cannot identify" rather than a placeholder.
[[nodiscard]] const char* format_mime_type(FileFormat format) noexcept;

/// The conventional lowercase extension WITHOUT a dot, e.g. "jpg" for Jpeg.
/// Advisory only: this is for display and for suggesting a filename, never
/// for detection. Never null; empty for Unknown.
[[nodiscard]] const char* format_primary_extension(FileFormat format) noexcept;

/// Does this build contain a sniffer for `format`?
[[nodiscard]] bool format_detection_is_implemented(FileFormat format) noexcept;

/// Why there is no sniffer. Returns nullptr for an implemented format, so
/// `if (const char* why = ...)` is the natural spelling. The reason is
/// specific to the format — "not implemented yet" is not a reason.
[[nodiscard]] const char* format_detection_not_implemented_reason(FileFormat format) noexcept;

/// Map a filename extension (with or without a leading dot, any case) to the
/// format it CLAIMS to be.
///
/// The result is a *hint*, never a detection. Spec section 8: "Never trust the
/// file extension alone." Nothing in this module assigns
/// `DetectionResult::format` from this function's output; it populates
/// `DetectionResult::extension_hint` so that a mismatch between the name and
/// the bytes can be reported to the caller, which is the security-relevant
/// signal (a `.jpg` whose bytes are a ZIP is worth surfacing).
[[nodiscard]] FileFormat format_from_extension(std::string_view extension) noexcept;

/// What this product can actually do with `format`, derived from the support
/// matrix in docs/file-format-support.md. See capabilities.h for the mapping
/// rules and for why some spec section 8 capabilities are granted to nothing.
[[nodiscard]] CapabilitySet format_capabilities(FileFormat format) noexcept;

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_FORMAT_H
