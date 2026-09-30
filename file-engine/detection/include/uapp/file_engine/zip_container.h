// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/zip_container.h — telling ZIP-based formats apart.
//
// ---------------------------------------------------------------------------
// THE PROBLEM THIS SOLVES
// ---------------------------------------------------------------------------
// DOCX, XLSX, PPTX, ODT, ODS, ODP, EPUB and a plain archive of holiday photos
// all begin with the same four bytes: `PK\x03\x04`. A sniffer that stops at
// the magic number reports "zip" for all of them, which is true and useless:
// the UI cannot offer "open in the document viewer" from it, and — worse —
// a `.docx` upload that is actually a plain ZIP full of executables would be
// indistinguishable from a real document.
//
// The four OOXML/ODF families are distinguished by what is INSIDE the
// container, and there are two different conventions:
//
//   OOXML (DOCX/XLSX/PPTX): a `[Content_Types].xml` part at the root, plus a
//       family-specific directory — `word/`, `xl/` or `ppt/`. The presence of
//       `word/document.xml` is what makes a file a DOCX.
//   ODF (ODT/ODS/ODP): a `mimetype` member that must be FIRST in the archive
//       and STORED (uncompressed), whose entire content is the media type
//       string. This is specified exactly so that a sniffer can read the type
//       at a fixed offset without inflating anything. We honour that design.
//
// ---------------------------------------------------------------------------
// WHERE THE MEMBER NAMES LIVE, AND WHY THAT SHAPES THE API
// ---------------------------------------------------------------------------
// The authoritative member list is the CENTRAL DIRECTORY, which sits at the
// END of a ZIP file. That is awkward for a detector: for a 2 GB archive we do
// not want the whole thing in memory, and for a partially-downloaded or
// streamed file the end may not exist yet.
//
// So this works from a `FileSample` (see detector.h) holding a HEAD window
// and, when available, a TAIL window, and it has two strategies:
//
//   1. Central directory (authoritative). Locate the end-of-central-directory
//      record by a backwards scan of the tail, then walk the fixed-size
//      entry headers. Used whenever the EOCD is inside the sample.
//   2. Local file headers (best effort). Walk forward from offset 0 through
//      the local headers in the head window. This is what lets a truncated,
//      streamed or head-only sample still be classified, and it is reported
//      as such via `ZipScanMethod` so the caller's confidence reflects it.
//
// Strategy 2 can be defeated by a crafted archive (a local header can lie
// about a name that the central directory contradicts). That is exactly why
// the method is reported rather than hidden: a security decision must not be
// taken on a local-header-only classification.
//
// ---------------------------------------------------------------------------
// HOSTILE-INPUT POSTURE
// ---------------------------------------------------------------------------
// Every field read here comes from the file. Concretely:
//   * all reads go through ByteSpan, which cannot read out of range;
//   * every offset+length is bounds-checked before use, never after;
//   * the entry walk is capped (kMaxEntriesScanned) so a directory claiming
//     4 billion entries costs a bounded number of iterations;
//   * member names are capped (kMaxMemberNameBytes) and never copied into a
//     fixed buffer;
//   * a forward walk must strictly advance, or it stops — a zero-length
//     entry cannot spin the loop forever;
//   * Zip64 sentinel values are DETECTED and cause a fallback, not a
//     wrong-but-confident answer.
// Nothing here inflates, allocates per entry, or opens a nested container.

#ifndef UAPP_FILE_ENGINE_ZIP_CONTAINER_H
#define UAPP_FILE_ENGINE_ZIP_CONTAINER_H

#include <cstddef>
#include <cstdint>

#include "uapp/file_engine/byte_span.h"
#include "uapp/file_engine/format.h"

namespace uapp::files {

/// A head window, an optional tail window, and the true total size. A whole
/// in-memory buffer is the degenerate case: head covers everything, tail is
/// empty.
struct FileSample final {
    ByteSpan      head;
    ByteSpan      tail;
    std::uint64_t tail_offset = 0;  ///< file offset of tail.data()
    std::uint64_t total_size  = 0;  ///< true size of the whole file

    /// Build the whole-file case, where head IS the file.
    [[nodiscard]] static FileSample whole(ByteSpan buffer) noexcept;

    /// True iff the sample covers every byte of the file.
    [[nodiscard]] bool is_complete() const noexcept {
        return total_size == static_cast<std::uint64_t>(head.size());
    }

    /// The `length` bytes at absolute file offset `offset`, if they lie
    /// wholly within one window. Returns an empty span otherwise — it never
    /// stitches across the gap between head and tail, because the bytes in
    /// that gap were never read.
    [[nodiscard]] ByteSpan at(std::uint64_t offset, std::size_t length) const noexcept;
};

/// How a classification was reached. Reported so the caller can weigh it.
enum class ZipScanMethod : std::int32_t {
    /// Not a ZIP at all, or nothing about it could be read.
    None = 0,
    /// The central directory was located and walked. Authoritative.
    CentralDirectory = 1,
    /// Local file headers were walked forward from offset 0. Best effort.
    LocalHeaders = 2,
    /// The ODF `mimetype` member was read from the first local header.
    MimetypeMember = 3,
};

struct ZipClassification final {
    /// Zip when it is a ZIP we could not classify further; Docx/Xlsx/Pptx/Odt
    /// when we could; Unknown when it is not a ZIP.
    FileFormat    format = FileFormat::Unknown;
    ZipScanMethod method = ZipScanMethod::None;

    /// The sample did not contain the end-of-central-directory record, so
    /// the authoritative member list was unavailable. True for a genuinely
    /// truncated file AND for a head-only sample of a large valid one —
    /// the detector cannot tell those apart, and does not pretend to.
    bool central_directory_unreachable = false;

    /// A `[Content_Types].xml` part was seen, i.e. this is an OOXML package,
    /// even if no family directory was recognised. Lets the caller say "this
    /// is an Office file of a kind we do not handle" instead of "zip".
    bool ooxml_package = false;

    /// A Zip64 sentinel was encountered. Zip64 record parsing is NOT
    /// IMPLEMENTED (see zip_container.cpp), so the scan fell back.
    bool zip64 = false;
};

/// True iff the bytes begin with any of the three ZIP local-file signatures.
/// Cheap, and safe on a one-byte buffer.
[[nodiscard]] bool looks_like_zip(ByteSpan head) noexcept;

/// Classify a ZIP-based container. Returns `format == Unknown` when the
/// sample is not a ZIP. Never reads outside `sample`.
[[nodiscard]] ZipClassification classify_zip(const FileSample& sample) noexcept;

// --- limits, public so tests can assert against them -----------------------

/// Maximum central-directory or local-header entries examined. Real OOXML
/// packages have tens of entries; this is three orders of magnitude of slack
/// while keeping a hostile directory bounded.
inline constexpr std::size_t kMaxEntriesScanned = 4096;

/// Longest member name compared. The ZIP field is 16-bit, so a name can claim
/// 65535 bytes; nothing we look for is longer than 32.
inline constexpr std::size_t kMaxMemberNameBytes = 512;

/// The EOCD record can be followed by a comment of up to 65535 bytes, so the
/// record itself starts no earlier than 22 + 65535 bytes from the end.
inline constexpr std::size_t kMaxEocdSearchBytes = 22 + 65535;

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_ZIP_CONTAINER_H
