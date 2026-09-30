// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/detector.h — "what is this file?"
//
// Implements the detection half of spec section 8. Detection ONLY: this
// module answers what a file is and what the product could do with it. It
// does not open, parse, render or convert anything, and it never will —
// those are pdf-engine/, ocr/ and the conversion engine, which will *call*
// this before trusting a filename.
//
// ===========================================================================
// 1. THE RULE: NEVER TRUST THE EXTENSION ALONE
// ===========================================================================
// Spec section 8 states it; this API enforces it structurally rather than by
// convention. `DetectionResult::format` is **only ever** assigned from the
// bytes. A filename can never promote a format into that field. What the name
// claimed goes into `extension_hint`, and a disagreement sets
// `extension_conflicts` — which is the security-interesting output, since an
// `invoice.pdf` whose bytes are a ZIP is worth surfacing to a human.
//
// The consequence is deliberate and worth stating plainly: for a format whose
// sniffer is not implemented (say MP3), `format` stays `Unknown` even when the
// name says `.mp3`. That is the honest answer. A caller who wants to act on
// the hint can, having been told it is a hint.
//
// ===========================================================================
// 2. THE API SHAPE: PATH *AND* BUFFER, WITH ONE ENGINE UNDER BOTH
// ===========================================================================
// A mobile file picker hands back a security-scoped URI or a byte array, not
// a POSIX path (architecture.md: "file picker URIs" are a platform service
// injected downward). A path-only detector is therefore unusable on the
// platform this product targets first. Equally, a buffer-only detector forces
// a 2 GB video into RAM to read eight magic bytes.
//
// So the engine is defined over a `FileSample` — a HEAD window, an optional
// TAIL window, and the true total size — and there are three entry points:
//
//   detect_sample(sample, hints, out)  pure, no I/O. The engine.
//   detect_buffer(bytes, hints, out)   whole file already in memory.
//   detect_path(path, out)             opens the file, reads bounded windows.
//
// `detect_path` reads at most kHeadSampleBytes from the front and, only when
// the file is larger than that, kTailSampleBytes from the end. It never reads
// the whole file. The tail exists for exactly one reason: a ZIP's member list
// lives in the central directory at the END of the file, so without it a
// 10 MB DOCX would be classified from local headers alone at lower
// confidence.
//
// `describe_*` goes one step further and produces the full spec section 8
// `File` value, which additionally requires a checksum over every byte.
//
// ===========================================================================
// 3. MALFORMED INPUT IS THE NORMAL CASE, NOT THE EXCEPTION
// ===========================================================================
// spec section 28/29 name corrupted, malformed and truncated files as a test
// class and treat them as security-sensitive. The contract here:
//
//   * A 0-byte input is `Status::FileEmpty`. It is NOT reported as "no match":
//     "there is nothing here" and "there is something here we do not
//     recognise" are different facts and callers act on them differently.
//   * A 1-byte, 3-byte or otherwise absurdly short input is a normal `Ok`
//     result. Every signature comparison goes through `ByteSpan`, which
//     cannot read past the end, so a 2-byte file checked against an 8-byte
//     PNG signature returns "no" rather than reading six bytes of adjacent
//     heap.
//   * A truncated or corrupt ZIP directory yields `FileFormat::Zip` with
//     `container_truncated == true` and reduced confidence — never a crash,
//     never a confident DOCX claim, and never a silent `Unknown` that hides
//     the fact that we know it is an archive.
//   * No input, of any shape, causes a read outside the caller's buffer.
//     That property is enforced by the type system (see byte_span.h), not by
//     forty hand-written length checks.

#ifndef UAPP_FILE_ENGINE_DETECTOR_H
#define UAPP_FILE_ENGINE_DETECTOR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "uapp/file_engine/byte_span.h"
#include "uapp/file_engine/file.h"
#include "uapp/file_engine/format.h"
#include "uapp/file_engine/zip_container.h"
#include "uapp/status.h"

namespace uapp::files {

// --- sampling limits -------------------------------------------------------

/// Bytes read from the front of a file. Generous enough for a PDF whose
/// `%PDF-` marker is preceded by junk, and for the local headers of a small
/// OOXML package, while staying a single comfortable read on Tier D.
inline constexpr std::size_t kHeadSampleBytes = 64u * 1024u;

/// Bytes read from the end. Must exceed the maximum distance from the end of
/// a ZIP to the start of its end-of-central-directory record (22 + a 65535
/// -byte comment), plus room for the directory itself in a typical package.
inline constexpr std::size_t kTailSampleBytes = 128u * 1024u;

/// Bytes examined by the UTF-8 / binary heuristic. A file is judged text on
/// its first 8 KiB; scanning a 2 GB file to decide whether to syntax
/// -highlight it would be absurd, and the matrix already documents the
/// heuristic nature of encoding detection (file-format-support.md §2, TXT).
inline constexpr std::size_t kTextProbeBytes = 8u * 1024u;

// --- result ----------------------------------------------------------------

/// How much the result should be believed. Ordered: higher is stronger.
enum class DetectionConfidence : std::int32_t {
    /// Nothing matched and the name said nothing either.
    None = 0,
    /// The bytes matched nothing; the filename extension is the ONLY signal.
    /// `format` is still Unknown — see the header. Read `extension_hint`.
    ExtensionOnly = 1,
    /// A content heuristic (not a signature) decided it. Plain text.
    Heuristic = 2,
    /// A magic number matched.
    Magic = 3,
    /// A magic number matched AND the container's own contents confirmed the
    /// specific subtype. The strongest answer this module produces.
    MagicAndContainer = 4,
};

[[nodiscard]] const char* detection_confidence_name(DetectionConfidence confidence) noexcept;

/// What produced the answer. Reported so that a caller making a security
/// decision can refuse to act on a best-effort classification.
enum class DetectionMethod : std::int32_t {
    NoMatch                = 0,
    MagicSignature         = 1,  ///< signature at offset 0
    MagicSignatureAtOffset = 2,  ///< signature found after leading bytes (PDF)
    TextHeuristic          = 3,
    ZipCentralDirectory    = 4,  ///< authoritative member list
    ZipLocalHeaders        = 5,  ///< best effort; see zip_container.h
    ZipMimetypeMember      = 6,  ///< the ODF `mimetype` member
    ExtensionHintOnly      = 7,
};

[[nodiscard]] const char* detection_method_name(DetectionMethod method) noexcept;

struct DetectionResult final {
    /// ALWAYS derived from bytes. Never from the filename.
    FileFormat format = FileFormat::Unknown;

    /// The outer container when `format` is a refinement of one, e.g. Zip
    /// for a Docx. Unknown otherwise.
    FileFormat container_format = FileFormat::Unknown;

    DetectionConfidence confidence = DetectionConfidence::None;
    DetectionMethod     method     = DetectionMethod::NoMatch;

    /// What the filename extension claimed, or Unknown if there was no name
    /// or the extension is not in the registry. Advisory.
    FileFormat extension_hint = FileFormat::Unknown;

    /// The name claimed one format and the bytes said another. Not an error
    /// — a renamed file is common and legitimate — but the one signal worth
    /// showing a human before they open an attachment.
    bool extension_conflicts = false;

    /// The container's authoritative member list could not be reached in the
    /// sample: truncated file, Zip64, or a head-only sample of a large one.
    /// The subtype claim, if any, came from local headers.
    bool container_truncated = false;

    /// An OOXML package (`[Content_Types].xml` present) whose family we did
    /// not recognise. `format` is Zip in that case.
    bool unrecognised_ooxml = false;

    /// Offset at which the signature was found. Non-zero only for formats
    /// whose spec tolerates leading bytes (PDF). Reported because "a PDF
    /// with 900 bytes of something else in front of it" is worth knowing.
    std::uint64_t magic_offset = 0;

    /// Convenience: the media type for `format`. Never null.
    [[nodiscard]] const char* mime_type() const noexcept { return format_mime_type(format); }

    /// True iff the bytes identified something. `false` covers both "no
    /// match" and "extension hint only".
    [[nodiscard]] bool identified() const noexcept {
        return format != FileFormat::Unknown;
    }
};

/// Secondary signals the caller can supply. All optional; detection works
/// with an entirely empty `DetectionHints`.
struct DetectionHints final {
    /// The display name, e.g. "Q3 report.docx". Only its extension is used,
    /// and only to populate `extension_hint`.
    std::string_view file_name;

    /// A media type asserted by the host (an HTTP Content-Type, an Android
    /// `ContentResolver.getType()`).
    ///
    /// NOT IMPLEMENTED: this field is accepted and IGNORED. It is declared
    /// now because the call sites that will supply it (the upload path and
    /// the Android picker) are being written against this header, and
    /// changing the struct later would churn them. Using it correctly means
    /// deciding a trust order between a host-asserted type and our own
    /// sniffing — a policy question for security-model.md, not a default to
    /// guess at. Today the bytes win unconditionally, which is the safe
    /// behaviour to be stuck with.
    std::string_view declared_mime_type;
};

// --- the engine ------------------------------------------------------------

/// Detect from a head (+ optional tail) window. Pure: no I/O, no allocation,
/// `noexcept`. This is the function every other entry point funnels into,
/// and the one a fuzz target should drive.
///
/// Returns Status::FileEmpty when the sample has no bytes at all. Otherwise
/// returns Status::Ok, with `out` describing what was found — including the
/// legitimate outcome of "nothing was found".
[[nodiscard]] Status detect_sample(const FileSample& sample, const DetectionHints& hints,
                                   DetectionResult& out) noexcept;

/// Detect from a complete in-memory buffer — the mobile / sandboxed case
/// where there is no filesystem path. Because the whole file is present, the
/// ZIP central directory is always reachable, so this yields the strongest
/// possible container classification.
[[nodiscard]] Status detect_buffer(ByteSpan buffer, const DetectionHints& hints,
                                   DetectionResult& out) noexcept;

/// Detect from a filesystem path. Reads at most kHeadSampleBytes from the
/// front and kTailSampleBytes from the end; never the whole file. The
/// filename is used as the extension hint automatically.
///
/// Status::FilePathInvalid for an empty path or one containing a NUL;
/// Status::FileOpenFailed if it cannot be opened; Status::FileEmpty for a
/// zero-length file; Status::FileReadFailed on a mid-read failure.
[[nodiscard]] Status detect_path(std::string_view path, DetectionResult& out);

// --- producing the full File value ----------------------------------------

/// Build the complete spec section 8 `File` from a buffer.
///
/// `id` is supplied by the caller rather than generated here, because
/// `uapp::IdSource` is a port (core/common/ports.h) and a detector that
/// reaches for ambient identity is untestable. Pass a nil id if the file is
/// not being persisted.
///
/// The checksum covers every byte of `buffer`. Timestamps are taken from
/// `created_at_ms` / `modified_at_ms`, which the caller gets from its
/// `uapp::Clock` port or from the host's file metadata — this function does
/// not read a clock either.
[[nodiscard]] Status describe_buffer(ByteSpan buffer, const DetectionHints& hints,
                                     const FileId& id, std::int64_t created_at_ms,
                                     std::int64_t modified_at_ms, File& out);

/// Build the complete `File` from a path. Unlike `detect_path` this DOES
/// stream the whole file, because the checksum requires every byte; it does
/// so in bounded chunks and never holds more than one chunk in memory.
/// Timestamps come from the filesystem where available.
[[nodiscard]] Status describe_path(std::string_view path, const FileId& id, File& out);

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_DETECTOR_H
