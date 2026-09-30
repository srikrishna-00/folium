// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/detector.h"

#include <cstddef>
#include <cstdint>

#include "uapp/file_engine/text_probe.h"

namespace uapp::files {
namespace {

// --- the signature table ---------------------------------------------------
//
// Only signatures whose format has a sniffer here. A signature for a format
// with no engine behind it would produce a detection result nothing can act
// on, and `format_detection_not_implemented_reason()` already gives callers a
// better answer than a bare format name would.

struct Signature final {
    const std::uint8_t* pattern;
    std::size_t         length;
    FileFormat          format;
};

// PNG: the 8-byte signature from the PNG spec §5.2. The \r\n and \x1a bytes
// exist to catch exactly the corruption modes (CRLF translation, DOS EOF)
// that a 4-byte check would miss, so all eight are compared.
constexpr std::uint8_t kPngMagic[]  = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
// JPEG: SOI (FFD8) followed by the first marker's FF. Three bytes rather
// than two — a bare FFD8 is a common false positive in binary data.
constexpr std::uint8_t kJpegMagic[] = {0xFF, 0xD8, 0xFF};
constexpr std::uint8_t kGif87Magic[] = {'G', 'I', 'F', '8', '7', 'a'};
constexpr std::uint8_t kGif89Magic[] = {'G', 'I', 'F', '8', '9', 'a'};

constexpr Signature kSignatures[] = {
    {kPngMagic, sizeof(kPngMagic), FileFormat::Png},
    {kJpegMagic, sizeof(kJpegMagic), FileFormat::Jpeg},
    {kGif87Magic, sizeof(kGif87Magic), FileFormat::Gif},
    {kGif89Magic, sizeof(kGif89Magic), FileFormat::Gif},
};

// PDF is not in that table because it is the one format here whose own spec
// tolerates leading bytes. ISO 32000-1 §7.5.2 requires `%PDF-` as the first
// line, but Annex H and every real-world reader accept a header appearing
// within the first 1024 bytes — files served with an HTTP preamble, or
// concatenated by a broken tool, are common enough that rejecting them would
// mean failing to detect files every other viewer opens. We scan that window
// and REPORT the offset rather than silently normalising it.
constexpr std::uint8_t kPdfMagic[]       = {'%', 'P', 'D', 'F', '-'};
constexpr std::size_t  kPdfSearchWindow  = 1024;

/// Formats that are physically ZIP archives. Used only to decide whether a
/// name/content disagreement is a real one.
bool format_is_zip_based(FileFormat format) noexcept {
    switch (format) {
        case FileFormat::Zip:
        case FileFormat::Docx:
        case FileFormat::Xlsx:
        case FileFormat::Pptx:
        case FileFormat::Odt:
        case FileFormat::Ods:
        case FileFormat::Odp:
        case FileFormat::Epub:
            return true;
        default:
            return false;
    }
}

/// Populate the extension hint. This is the ONLY place a filename touches
/// the result, and it never writes `out.format`.
void apply_extension_hint(const DetectionHints& hints, DetectionResult& out) noexcept {
    if (hints.file_name.empty()) { return; }
    const std::string extension = extension_of(hints.file_name);
    if (extension.empty()) { return; }
    out.extension_hint = format_from_extension(extension);
}

/// Decide whether the name and the bytes disagree, once both are known.
void resolve_conflict(DetectionResult& out) noexcept {
    if (out.extension_hint == FileFormat::Unknown) { return; }

    if (out.format == FileFormat::Unknown) {
        // The bytes said nothing. That is not a conflict — it is the absence
        // of a second opinion. The confidence field records that the hint is
        // all we have.
        out.confidence = DetectionConfidence::ExtensionOnly;
        out.method     = DetectionMethod::ExtensionHintOnly;
        return;
    }

    if (out.extension_hint == out.format) { return; }

    // A ZIP-based format named by its generic container extension is not a
    // lie: a .docx IS a .zip, and flagging that would make the conflict
    // signal noise. Note the asymmetry — this exempts "detected Docx, named
    // .zip", NOT "detected a plain Zip, named .jpg".
    if (out.container_format == FileFormat::Zip && out.extension_hint == FileFormat::Zip) {
        return;
    }

    // The converse — the name claims a ZIP-based format and all we could see
    // was a generic ZIP — is a conflict only when we actually READ the member
    // list. If the container was truncated we never saw the part that would
    // have confirmed the claim, so the file is unproven, not guilty.
    if (out.format == FileFormat::Zip && out.container_truncated &&
        format_is_zip_based(out.extension_hint)) {
        return;
    }

    // Source code and plain text are the same bytes by definition; a .py
    // file detecting as PlainText is correct, not a mismatch.
    if (out.format == FileFormat::PlainText &&
        (out.extension_hint == FileFormat::SourceCode ||
         format_category(out.extension_hint) == FormatCategory::Code)) {
        return;
    }

    out.extension_conflicts = true;
}

/// Byte-signature pass. Returns true and fills `out` on a match.
bool match_signature(const FileSample& sample, DetectionResult& out) noexcept {
    for (const Signature& sig : kSignatures) {
        if (sample.head.starts_with(sig.pattern, sig.length)) {
            out.format     = sig.format;
            out.confidence = DetectionConfidence::Magic;
            out.method     = DetectionMethod::MagicSignature;
            return true;
        }
    }

    // PDF, with its tolerated preamble.
    const std::size_t window =
        sample.head.size() < kPdfSearchWindow ? sample.head.size() : kPdfSearchWindow;
    if (window >= sizeof(kPdfMagic)) {
        const std::size_t last = window - sizeof(kPdfMagic);
        for (std::size_t i = 0; i <= last; ++i) {
            if (sample.head.equal_at(i, kPdfMagic, sizeof(kPdfMagic))) {
                out.format       = FileFormat::Pdf;
                out.confidence   = DetectionConfidence::Magic;
                out.method       = (i == 0) ? DetectionMethod::MagicSignature
                                            : DetectionMethod::MagicSignatureAtOffset;
                out.magic_offset = i;
                return true;
            }
        }
    }
    return false;
}

DetectionMethod method_for(ZipScanMethod scan) noexcept {
    switch (scan) {
        case ZipScanMethod::CentralDirectory: return DetectionMethod::ZipCentralDirectory;
        case ZipScanMethod::LocalHeaders:     return DetectionMethod::ZipLocalHeaders;
        case ZipScanMethod::MimetypeMember:   return DetectionMethod::ZipMimetypeMember;
        case ZipScanMethod::None:             break;
    }
    return DetectionMethod::MagicSignature;
}

/// ZIP pass: the magic says "archive", the contents say which kind.
bool match_zip(const FileSample& sample, DetectionResult& out) noexcept {
    if (!looks_like_zip(sample.head)) { return false; }

    const ZipClassification zip = classify_zip(sample);
    out.format           = zip.format;
    out.container_format = FileFormat::Zip;
    out.container_truncated = zip.central_directory_unreachable;
    out.unrecognised_ooxml  = zip.ooxml_package && zip.format == FileFormat::Zip;
    out.method              = method_for(zip.method);

    if (zip.format != FileFormat::Zip && zip.method != ZipScanMethod::None) {
        // We named a specific subtype from the container's own contents.
        // MagicAndContainer only when that came from an authoritative source;
        // a local-header walk can be defeated by a crafted archive
        // (zip_container.h), so it stays at Magic and the caller sees
        // `container_truncated`.
        out.confidence = (zip.method == ZipScanMethod::LocalHeaders)
                             ? DetectionConfidence::Magic
                             : DetectionConfidence::MagicAndContainer;
    } else {
        // Just a ZIP — which is itself a confident answer from a magic
        // number, whether or not we could enumerate its members.
        out.confidence = DetectionConfidence::Magic;
    }
    return true;
}

}  // namespace

const char* detection_confidence_name(DetectionConfidence confidence) noexcept {
    switch (confidence) {
        case DetectionConfidence::None:              return "none";
        case DetectionConfidence::ExtensionOnly:     return "extension_only";
        case DetectionConfidence::Heuristic:         return "heuristic";
        case DetectionConfidence::Magic:             return "magic";
        case DetectionConfidence::MagicAndContainer: return "magic_and_container";
    }
    return "unknown";
}

const char* detection_method_name(DetectionMethod method) noexcept {
    switch (method) {
        case DetectionMethod::NoMatch:                return "no_match";
        case DetectionMethod::MagicSignature:         return "magic_signature";
        case DetectionMethod::MagicSignatureAtOffset: return "magic_signature_at_offset";
        case DetectionMethod::TextHeuristic:          return "text_heuristic";
        case DetectionMethod::ZipCentralDirectory:    return "zip_central_directory";
        case DetectionMethod::ZipLocalHeaders:        return "zip_local_headers";
        case DetectionMethod::ZipMimetypeMember:      return "zip_mimetype_member";
        case DetectionMethod::ExtensionHintOnly:      return "extension_hint_only";
    }
    return "unknown";
}

Status detect_sample(const FileSample& sample, const DetectionHints& hints,
                     DetectionResult& out) noexcept {
    out = DetectionResult();

    // Zero bytes is a distinct, reportable fact — not "unrecognised". A
    // caller re-indexing a library needs to tell a truncated download from a
    // file in a format we do not know.
    if (sample.head.empty() && sample.total_size == 0) { return Status::FileEmpty; }

    apply_extension_hint(hints, out);

    // Order is deliberate and is the substance of "never trust the
    // extension": containers first (they are the most specific answer),
    // then plain signatures, then the content heuristic, and the filename
    // never competes with any of them.
    if (!match_zip(sample, out)) {
        if (!match_signature(sample, out)) {
            if (probe_is_utf8_text(sample.head.subspan(0, kTextProbeBytes),
                                   sample.is_complete())) {
                out.format     = FileFormat::PlainText;
                out.confidence = DetectionConfidence::Heuristic;
                out.method     = DetectionMethod::TextHeuristic;
            }
        }
    }

    resolve_conflict(out);
    return Status::Ok;
}

Status detect_buffer(ByteSpan buffer, const DetectionHints& hints,
                     DetectionResult& out) noexcept {
    return detect_sample(FileSample::whole(buffer), hints, out);
}

}  // namespace uapp::files
