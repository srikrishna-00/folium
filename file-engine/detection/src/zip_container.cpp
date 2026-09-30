// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/zip_container.h"

#include <cstring>

namespace uapp::files {
namespace {

// APPNOTE.TXT signatures, little-endian on disk.
constexpr std::uint8_t kLocalFileHeaderSig[4]   = {0x50, 0x4B, 0x03, 0x04};
constexpr std::uint8_t kEmptyArchiveSig[4]      = {0x50, 0x4B, 0x05, 0x06};
constexpr std::uint8_t kSpannedArchiveSig[4]    = {0x50, 0x4B, 0x07, 0x08};
constexpr std::uint8_t kCentralDirEntrySig[4]   = {0x50, 0x4B, 0x01, 0x02};
constexpr std::uint8_t kZip64EocdLocatorSig[4]  = {0x50, 0x4B, 0x06, 0x07};

// Fixed portions, per APPNOTE.TXT sections 4.3.7, 4.3.12 and 4.3.16.
constexpr std::size_t kLocalHeaderFixedBytes    = 30;
constexpr std::size_t kCentralEntryFixedBytes   = 46;
constexpr std::size_t kEocdFixedBytes           = 22;

// Field offsets we actually read.
constexpr std::size_t kLocalCompressionOffset   = 8;
constexpr std::size_t kLocalCompressedSizeOff   = 18;
constexpr std::size_t kLocalNameLenOffset       = 26;
constexpr std::size_t kLocalExtraLenOffset      = 28;
constexpr std::size_t kLocalFlagsOffset         = 6;

constexpr std::size_t kCentralNameLenOffset     = 28;
constexpr std::size_t kCentralExtraLenOffset    = 30;
constexpr std::size_t kCentralCommentLenOffset  = 32;

constexpr std::size_t kEocdEntryCountOffset     = 10;
constexpr std::size_t kEocdCdSizeOffset         = 12;
constexpr std::size_t kEocdCdOffsetOffset       = 16;

/// Bit 3 of the general-purpose flags: sizes live in a trailing data
/// descriptor and the header's own size fields are zero. When it is set we
/// cannot compute where the next local header begins, so the forward walk
/// must stop rather than guess.
constexpr std::uint16_t kFlagDataDescriptor = 0x0008;

constexpr std::uint32_t kZip64Sentinel32 = 0xFFFFFFFFu;
constexpr std::uint16_t kZip64Sentinel16 = 0xFFFFu;

/// The ODF `mimetype` member content for the one ODF format whose detection
/// is implemented. ODS/ODP are deliberately absent — see
/// format_detection_not_implemented_reason(Ods).
constexpr char kOdtMimetype[] = "application/vnd.oasis.opendocument.text";

/// What a member name tells us. Checked longest-prefix-first by the caller.
FileFormat format_from_member_name(std::string_view name) noexcept {
    // The authoritative part paths. A file is a DOCX because it contains
    // word/document.xml, not because it contains a `word/` folder — but a
    // package can legally place the main part elsewhere and point at it from
    // _rels/.rels, so the directory prefix is kept as a secondary signal by
    // the caller.
    if (name == "word/document.xml") { return FileFormat::Docx; }
    if (name == "xl/workbook.xml") { return FileFormat::Xlsx; }
    if (name == "ppt/presentation.xml") { return FileFormat::Pptx; }
    return FileFormat::Unknown;
}

FileFormat format_from_member_prefix(std::string_view name) noexcept {
    if (name.rfind("word/", 0) == 0) { return FileFormat::Docx; }
    if (name.rfind("xl/", 0) == 0) { return FileFormat::Xlsx; }
    if (name.rfind("ppt/", 0) == 0) { return FileFormat::Pptx; }
    return FileFormat::Unknown;
}

/// Accumulates evidence across the entries we see, so one pass suffices.
struct Evidence final {
    FileFormat exact  = FileFormat::Unknown;  ///< an authoritative part path
    FileFormat prefix = FileFormat::Unknown;  ///< a family directory
    bool       content_types = false;

    void observe(std::string_view name) noexcept {
        if (name == "[Content_Types].xml") { content_types = true; }
        if (exact == FileFormat::Unknown) { exact = format_from_member_name(name); }
        if (prefix == FileFormat::Unknown) { prefix = format_from_member_prefix(name); }
    }

    [[nodiscard]] FileFormat verdict() const noexcept {
        if (exact != FileFormat::Unknown) { return exact; }
        if (prefix != FileFormat::Unknown) { return prefix; }
        return FileFormat::Unknown;
    }

    /// True once we hold an authoritative answer and further entries cannot
    /// change it. Lets a 10 000-entry XLSX stop after a handful.
    [[nodiscard]] bool conclusive() const noexcept { return exact != FileFormat::Unknown; }
};

/// Read the ODF `mimetype` member, which the ODF spec requires to be the
/// FIRST entry and STORED. Returns Unknown unless all of that holds — an
/// archive that merely happens to contain a deflated `mimetype` somewhere is
/// not an ODF package and must not be treated as one.
FileFormat read_odf_mimetype(const FileSample& sample) noexcept {
    const ByteSpan header = sample.at(0, kLocalHeaderFixedBytes);
    if (header.size() < kLocalHeaderFixedBytes) { return FileFormat::Unknown; }
    if (!header.starts_with(kLocalFileHeaderSig, sizeof(kLocalFileHeaderSig))) {
        return FileFormat::Unknown;
    }

    std::uint16_t method = 0;
    std::uint16_t name_len = 0;
    std::uint16_t extra_len = 0;
    std::uint32_t compressed = 0;
    if (!header.read_u16le(kLocalCompressionOffset, method) ||
        !header.read_u16le(kLocalNameLenOffset, name_len) ||
        !header.read_u16le(kLocalExtraLenOffset, extra_len) ||
        !header.read_u32le(kLocalCompressedSizeOff, compressed)) {
        return FileFormat::Unknown;
    }
    if (method != 0) { return FileFormat::Unknown; }  // must be STORED
    if (name_len != 8) { return FileFormat::Unknown; }  // strlen("mimetype")

    const ByteSpan name = sample.at(kLocalHeaderFixedBytes, name_len);
    if (name.text_at(0, name.size()) != "mimetype") { return FileFormat::Unknown; }

    // Cap the declared content length before using it as a read length: a
    // corrupt header may claim 4 GiB.
    if (compressed == 0 || compressed > 256) { return FileFormat::Unknown; }

    const std::uint64_t content_offset =
        static_cast<std::uint64_t>(kLocalHeaderFixedBytes) + name_len + extra_len;
    const ByteSpan content = sample.at(content_offset, compressed);
    if (content.size() != compressed) { return FileFormat::Unknown; }

    const std::string_view media_type = content.text_at(0, content.size());
    if (media_type == kOdtMimetype) { return FileFormat::Odt; }

    // A valid ODF package of a type whose detection is not implemented
    // (ODS/ODP). Caller reports it as a plain Zip, which is honest.
    return FileFormat::Unknown;
}

/// Outcome of the central-directory strategy. "Located but uninformative"
/// (an ordinary archive of ordinary files) is a *different* answer from
/// "could not be located", and only the second should lower confidence.
enum class CentralDirOutcome : std::int32_t {
    Unreachable = 0,  ///< no EOCD in the sample, or it was unusable
    Zip64       = 1,  ///< Zip64 sentinels; parsing NOT IMPLEMENTED
    Scanned     = 2,  ///< walked; `out` holds whatever it found (maybe nothing)
};

/// Strategy 1: walk the central directory. `out` is only touched when the
/// outcome is Scanned.
CentralDirOutcome scan_central_directory(const FileSample& sample, Evidence& out) noexcept {
    // The EOCD lies in the last 22 + 65535 bytes. Search the tail window if
    // there is one, otherwise the head (which, for a complete small file,
    // is the whole file).
    const ByteSpan      window      = sample.tail.empty() ? sample.head : sample.tail;
    const std::uint64_t window_base = sample.tail.empty() ? 0 : sample.tail_offset;
    if (window.size() < kEocdFixedBytes) { return CentralDirOutcome::Unreachable; }

    const std::size_t search_len =
        window.size() < kMaxEocdSearchBytes ? window.size() : kMaxEocdSearchBytes;
    const ByteSpan search = window.subspan(window.size() - search_len, search_len);

    std::size_t rel = 0;
    if (!search.rfind(kEmptyArchiveSig, sizeof(kEmptyArchiveSig), rel)) {
        return CentralDirOutcome::Unreachable;
    }
    const std::size_t eocd_in_window = (window.size() - search_len) + rel;

    std::uint16_t entry_count = 0;
    std::uint32_t cd_size     = 0;
    std::uint32_t cd_offset   = 0;
    if (!window.read_u16le(eocd_in_window + kEocdEntryCountOffset, entry_count) ||
        !window.read_u32le(eocd_in_window + kEocdCdSizeOffset, cd_size) ||
        !window.read_u32le(eocd_in_window + kEocdCdOffsetOffset, cd_offset)) {
        // The EOCD signature was there but the record is truncated: the file
        // is damaged at exactly the point we need. Not a usable directory.
        return CentralDirOutcome::Unreachable;
    }

    // Zip64: the 32-bit fields saturate and the real values live in a Zip64
    // EOCD record found via its locator.
    //
    // NOT IMPLEMENTED: Zip64 end-of-central-directory parsing. A Zip64 OOXML
    // package (> 4 GiB or > 65535 parts) therefore falls back to the local
    // header walk instead of being misread — the sentinel values would
    // otherwise point the walk at offset 0xFFFFFFFF. Flagged so the caller
    // can lower confidence rather than silently accept the fallback.
    if (cd_offset == kZip64Sentinel32 || cd_size == kZip64Sentinel32 ||
        entry_count == kZip64Sentinel16 ||
        (eocd_in_window >= 20 &&
         window.equal_at(eocd_in_window - 20, kZip64EocdLocatorSig,
                         sizeof(kZip64EocdLocatorSig)))) {
        return CentralDirOutcome::Zip64;
    }

    // An archive with no entries. Well-formed, and conclusively not an OOXML
    // or ODF package, so report it as scanned-and-empty rather than broken.
    if (cd_size == 0 || entry_count == 0) { return CentralDirOutcome::Scanned; }

    // The central directory must lie within the file, and must end at or
    // before the EOCD record that describes it. A corrupt or crafted offset
    // is rejected rather than clamped: clamping would start parsing entry
    // headers at arbitrary file content.
    const std::uint64_t cd_end   = static_cast<std::uint64_t>(cd_offset) + cd_size;
    const std::uint64_t eocd_abs = window_base + eocd_in_window;
    if (cd_end > sample.total_size || cd_end > eocd_abs) {
        return CentralDirOutcome::Unreachable;
    }

    Evidence evidence;
    std::uint64_t offset  = cd_offset;
    std::size_t   scanned = 0;
    const std::size_t limit =
        entry_count < kMaxEntriesScanned ? entry_count : kMaxEntriesScanned;

    while (scanned < limit) {
        const ByteSpan entry = sample.at(offset, kCentralEntryFixedBytes);
        if (entry.size() < kCentralEntryFixedBytes) { break; }  // outside the sample
        if (!entry.starts_with(kCentralDirEntrySig, sizeof(kCentralDirEntrySig))) { break; }

        std::uint16_t name_len    = 0;
        std::uint16_t extra_len   = 0;
        std::uint16_t comment_len = 0;
        if (!entry.read_u16le(kCentralNameLenOffset, name_len) ||
            !entry.read_u16le(kCentralExtraLenOffset, extra_len) ||
            !entry.read_u16le(kCentralCommentLenOffset, comment_len)) {
            break;
        }

        if (name_len > 0) {
            const std::size_t compare_len =
                name_len < kMaxMemberNameBytes ? name_len : kMaxMemberNameBytes;
            const ByteSpan name_bytes =
                sample.at(offset + kCentralEntryFixedBytes, compare_len);
            if (name_bytes.size() == compare_len) {
                evidence.observe(name_bytes.text_at(0, compare_len));
            }
        }
        if (evidence.conclusive()) { break; }

        // 46 + three 16-bit fields cannot overflow a uint64.
        offset += static_cast<std::uint64_t>(kCentralEntryFixedBytes) + name_len + extra_len +
                  comment_len;
        if (offset >= cd_end) { break; }
        ++scanned;
    }

    out = evidence;
    return CentralDirOutcome::Scanned;
}

/// Strategy 2: walk local file headers forward from offset 0.
bool scan_local_headers(const FileSample& sample, Evidence& out) noexcept {
    Evidence      evidence;
    std::uint64_t offset  = 0;
    std::size_t   scanned = 0;

    while (scanned < kMaxEntriesScanned) {
        const ByteSpan header = sample.at(offset, kLocalHeaderFixedBytes);
        if (header.size() < kLocalHeaderFixedBytes) { break; }
        if (!header.starts_with(kLocalFileHeaderSig, sizeof(kLocalFileHeaderSig))) { break; }

        std::uint16_t flags      = 0;
        std::uint16_t name_len   = 0;
        std::uint16_t extra_len  = 0;
        std::uint32_t compressed = 0;
        if (!header.read_u16le(kLocalFlagsOffset, flags) ||
            !header.read_u16le(kLocalNameLenOffset, name_len) ||
            !header.read_u16le(kLocalExtraLenOffset, extra_len) ||
            !header.read_u32le(kLocalCompressedSizeOff, compressed)) {
            break;
        }

        if (name_len > 0) {
            const std::size_t compare_len =
                name_len < kMaxMemberNameBytes ? name_len : kMaxMemberNameBytes;
            const ByteSpan name_bytes =
                sample.at(offset + kLocalHeaderFixedBytes, compare_len);
            if (name_bytes.size() == compare_len) {
                evidence.observe(name_bytes.text_at(0, compare_len));
            }
        }
        if (evidence.conclusive()) { break; }

        // Bit 3 means the header's size fields are zero and the real sizes
        // follow the data. Without them the next header's offset is unknown,
        // and searching for the next signature would happily find one inside
        // compressed data. Stop instead of guessing.
        if ((flags & kFlagDataDescriptor) != 0) { break; }
        if (compressed == kZip64Sentinel32) { break; }

        const std::uint64_t next = offset + kLocalHeaderFixedBytes + name_len + extra_len +
                                   compressed;
        if (next <= offset) { break; }  // must strictly advance
        offset = next;
        ++scanned;
    }

    if (evidence.verdict() == FileFormat::Unknown && !evidence.content_types) { return false; }
    out = evidence;
    return true;
}

}  // namespace

FileSample FileSample::whole(ByteSpan buffer) noexcept {
    FileSample s;
    s.head        = buffer;
    s.total_size  = buffer.size();
    s.tail_offset = 0;
    return s;
}

ByteSpan FileSample::at(std::uint64_t offset, std::size_t length) const noexcept {
    if (length == 0) { return ByteSpan(); }

    // Head window: [0, head.size()).
    if (offset < static_cast<std::uint64_t>(head.size())) {
        const std::size_t head_offset = static_cast<std::size_t>(offset);
        if (head.covers(head_offset, length)) { return head.subspan(head_offset, length); }
        // Falls through: the range may still be fully inside the tail.
    }

    // Tail window: [tail_offset, tail_offset + tail.size()).
    if (!tail.empty() && offset >= tail_offset) {
        const std::uint64_t rel = offset - tail_offset;
        if (rel <= static_cast<std::uint64_t>(tail.size())) {
            const std::size_t tail_offset_in = static_cast<std::size_t>(rel);
            if (tail.covers(tail_offset_in, length)) {
                return tail.subspan(tail_offset_in, length);
            }
        }
    }

    // The range straddles the unread gap, or lies past the end. Never
    // stitched: those bytes were never read, so pretending otherwise would
    // compare against uninitialised or unrelated memory.
    return ByteSpan();
}

bool looks_like_zip(ByteSpan head) noexcept {
    return head.starts_with(kLocalFileHeaderSig, sizeof(kLocalFileHeaderSig)) ||
           head.starts_with(kEmptyArchiveSig, sizeof(kEmptyArchiveSig)) ||
           head.starts_with(kSpannedArchiveSig, sizeof(kSpannedArchiveSig));
}

ZipClassification classify_zip(const FileSample& sample) noexcept {
    ZipClassification result;
    if (!looks_like_zip(sample.head)) { return result; }

    // It is at least a ZIP. Anything below can only refine this.
    result.format = FileFormat::Zip;

    // ODF first: its `mimetype` member is specified to be first and stored
    // precisely so a sniffer can read it without a directory walk, and it is
    // cheaper and more definitive than either strategy below.
    const FileFormat odf = read_odf_mimetype(sample);
    if (odf != FileFormat::Unknown) {
        result.format = odf;
        result.method = ZipScanMethod::MimetypeMember;
        return result;
    }

    Evidence                central;
    const CentralDirOutcome outcome = scan_central_directory(sample, central);
    if (outcome == CentralDirOutcome::Scanned) {
        // Authoritative, even when it found nothing interesting: a directory
        // we walked in full that holds no OOXML part means this really is
        // just a ZIP.
        result.method        = ZipScanMethod::CentralDirectory;
        result.ooxml_package = central.content_types;
        if (central.verdict() != FileFormat::Unknown) { result.format = central.verdict(); }
        return result;
    }
    result.zip64                        = (outcome == CentralDirOutcome::Zip64);
    result.central_directory_unreachable = true;

    Evidence local;
    if (scan_local_headers(sample, local)) {
        result.method        = ZipScanMethod::LocalHeaders;
        result.ooxml_package = local.content_types;
        if (local.verdict() != FileFormat::Unknown) { result.format = local.verdict(); }
        return result;
    }

    // A ZIP whose members we could not enumerate: truncated, encrypted
    // directory, Zip64, or simply an archive of unremarkable files.
    result.method = ZipScanMethod::None;
    return result;
}

}  // namespace uapp::files
