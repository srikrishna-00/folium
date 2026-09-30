// SPDX-License-Identifier: Apache-2.0
//
// tests/fuzz/fuzz_file_detection.cpp — the `fuzz_detect` target from
// testing-strategy.md §6.1 ("arbitrary bytes / P0 / Must never crash on
// anything, by definition").
//
// ===========================================================================
// WHAT IS BEING FUZZED, AND WHY THIS ENTRY POINT
// ===========================================================================
// `uapp::files::detect_buffer` is the *untrusted-upload* entry point: raw
// bytes, no filesystem, no path, no provenance. It is what the mobile picker
// and the eventual HTTP upload handler call, and it is the FIRST first-party
// code that ever touches an attacker-supplied file — before pdf-engine/, ocr/
// or any conversion path is allowed to look at it (file-engine/CMakeLists.txt
// states that ordering explicitly). A memory-safety bug here is reachable by
// anyone who can hand the product a file.
//
// `detect_path` is deliberately NOT fuzzed. It is a thin bounded-read wrapper
// isolated in one translation unit (detector_path.cpp) whose only job is to
// fill a FileSample and hand it to the same engine; fuzzing it would spend
// every cycle in open(2) and re-test this code through a slower door.
//
// ===========================================================================
// THE INPUT IS THE FILE, VERBATIM
// ===========================================================================
// libFuzzer's byte string IS the uploaded file's bytes. Nothing is stripped,
// no length prefix, no header, no embedded control fields. That is a
// deliberate constraint rather than a simplification:
//
//   * every corpus entry under tests/fixtures/corpus/fuzz_file_detection/ is
//     a real file of its claimed format, so `-merge=1` minimisation and
//     `-print_final_stats` mean what they say;
//   * a crashing input dumped by libFuzzer is directly usable as a
//     regression fixture (testing-strategy.md §6.2, "every crash becomes a
//     regression test") without a decoder step in between;
//   * nobody can later mistake a harness framing bug for a detector bug.
//
// The one thing the file bytes cannot carry is the FILENAME, which
// `detect_buffer` also consumes (as an advisory hint only — detector.h §1).
// Leaving the hint path unfuzzed would leave `extension_of`, the registry
// lookup and the whole conflict-resolution ladder untested by this target.
// So the harness runs the same bytes a second time with one name drawn
// deterministically from a fixed table, indexed by the input's own first
// byte. Deterministic, so libFuzzer's reproduction guarantee holds; and it
// does not touch the corpus file format, because the name is derived, not
// embedded.
//
// ===========================================================================
// THE ORACLE: CRASHES ARE NOT THE ONLY BUG
// ===========================================================================
// ASan/UBSan catch memory and UB. They do not catch "detection returned a
// self-contradictory verdict", which for this module is the more likely
// defect — and a wrong verdict here is a security finding, not a cosmetic
// one, because callers gate on `confidence` and `extension_conflicts`.
//
// So every input is also checked against the invariants detector.h states in
// prose. ONLY invariants the public header actually promises are asserted
// here; inventing a stricter rule than the contract would turn a harness
// opinion into a fake product defect. Each check below cites its source.
//
// A violation calls std::abort() so libFuzzer records it exactly as it
// records a sanitizer fault. -fno-exceptions (ADR-0001) means there is no
// throwing alternative, and that is fine: an oracle failure IS a crash.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "uapp/file_engine/byte_span.h"
#include "uapp/file_engine/capabilities.h"
#include "uapp/file_engine/checksum.h"
#include "uapp/file_engine/detector.h"
#include "uapp/file_engine/file.h"
#include "uapp/file_engine/format.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::files::ByteSpan;
using uapp::files::DetectionConfidence;
using uapp::files::DetectionHints;
using uapp::files::DetectionMethod;
using uapp::files::DetectionResult;
using uapp::files::File;
using uapp::files::FileFormat;
using uapp::files::FileId;

[[noreturn]] void oracle_failed(const char* rule, const char* detail) {
    std::fprintf(stderr,
                 "\n*** fuzz_file_detection ORACLE VIOLATION ***\n"
                 "  invariant: %s\n"
                 "  detail   : %s\n",
                 rule, detail == nullptr ? "(none)" : detail);
    std::fflush(stderr);
    std::abort();
}

#define ORACLE(cond, detail)                          \
    do {                                              \
        if (!(cond)) { oracle_failed(#cond, detail); } \
    } while (false)

/// Names chosen so the hint path is exercised across its whole ladder:
/// agreement, a real conflict, the ZIP-family exemption, the source-code
/// exemption, a name with no extension at all, and two hostile shapes.
const char* const kNames[] = {
    "",                       // no name: hint path must stay inert
    "upload.pdf",             // agrees with a PDF payload
    "vacation.jpg",           // the classic renamed-archive conflict
    "package.zip",            // the "a .docx IS a .zip" exemption
    "q3.docx",                // claims a ZIP subtype
    "tool.py",                // the source-code / plain-text exemption
    "song.mp3",               // a registry format with no sniffer
    ".bashrc",                // a dotfile has no extension
    "archive.tar.gz",         // double extension
    "C:\\docs\\budget.xlsx",  // Windows separators
    "noextension",
    "trailing.",
};
constexpr std::size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);

bool format_is_zip_family(FileFormat f) noexcept {
    switch (f) {
        case FileFormat::Zip:
        case FileFormat::Docx:
        case FileFormat::Xlsx:
        case FileFormat::Pptx:
        case FileFormat::Odt:
            return true;
        default:
            return false;
    }
}

/// Invariants from detector.h. `named` says whether a filename hint was
/// supplied, because several rules are only meaningful without one.
void check_detection_invariants(const DetectionResult& r, std::size_t input_size, bool named) {
    // detector.h: "Convenience: the media type for `format`. Never null."
    ORACLE(r.mime_type() != nullptr, "mime_type() returned null");
    ORACLE(uapp::files::format_name(r.format) != nullptr, "format_name() returned null");
    ORACLE(uapp::files::detection_confidence_name(r.confidence) != nullptr, "confidence name");
    ORACLE(uapp::files::detection_method_name(r.method) != nullptr, "method name");

    // detector.h §1: the filename can NEVER promote a format. The two states
    // that mean "the name was the only evidence" must therefore both leave
    // `format` Unknown — this is the structural no-extension-trust rule, and
    // it is the single most security-relevant property of this module.
    if (r.method == DetectionMethod::ExtensionHintOnly) {
        ORACLE(r.format == FileFormat::Unknown,
               "method is ExtensionHintOnly but a format was claimed");
    }
    if (r.confidence == DetectionConfidence::ExtensionOnly) {
        ORACLE(r.format == FileFormat::Unknown,
               "confidence is ExtensionOnly but a format was claimed");
    }

    // detector.h: `identified()` is "true iff the bytes identified something",
    // and None is documented as "nothing matched and the name said nothing".
    if (r.identified()) {
        ORACLE(r.confidence != DetectionConfidence::None,
               "a format was identified at confidence None");
        ORACLE(r.method != DetectionMethod::NoMatch,
               "a format was identified by method NoMatch");
    }

    // detector.h: magic_offset is "the offset at which the signature was
    // found ... non-zero only for formats whose spec tolerates leading bytes
    // (PDF)". An offset outside the buffer would mean we read out of it.
    ORACLE(r.magic_offset <= static_cast<std::uint64_t>(input_size),
           "magic_offset points past the end of the input");
    if (r.magic_offset != 0) {
        ORACLE(r.format == FileFormat::Pdf, "a non-PDF format reported a magic offset");
        ORACLE(r.method == DetectionMethod::MagicSignatureAtOffset, "offset without the method");
    }

    // detector.h: container_format is "the outer container when `format` is a
    // refinement of one, e.g. Zip for a Docx", and the two container flags
    // describe a ZIP walk. None of the three can be set for a PNG.
    if (r.container_truncated || r.unrecognised_ooxml ||
        r.container_format != FileFormat::Unknown) {
        ORACLE(format_is_zip_family(r.format),
               "container state set on a result that is not a ZIP-family format");
    }
    if (r.unrecognised_ooxml) {
        // detector.h: "`format` is Zip in that case."
        ORACLE(r.format == FileFormat::Zip, "unrecognised_ooxml set on a named subtype");
    }

    if (!named) {
        // With no name there is nothing to hint with and nothing to conflict
        // with. apply_extension_hint() returns early; if either of these ever
        // becomes true, a filename leaked in from somewhere it should not.
        ORACLE(r.extension_hint == FileFormat::Unknown, "hint set without a filename");
        ORACLE(!r.extension_conflicts, "conflict flagged without a filename");
    }
    // A conflict is by definition a disagreement between two opinions, so it
    // requires a hint to disagree with (resolve_conflict returns early when
    // there is none).
    if (r.extension_conflicts) {
        ORACLE(r.extension_hint != FileFormat::Unknown, "conflict with no hinted format");
    }
}

bool same_result(const DetectionResult& a, const DetectionResult& b) noexcept {
    return a.format == b.format && a.container_format == b.container_format &&
           a.confidence == b.confidence && a.method == b.method &&
           a.extension_hint == b.extension_hint &&
           a.extension_conflicts == b.extension_conflicts &&
           a.container_truncated == b.container_truncated &&
           a.unrecognised_ooxml == b.unrecognised_ooxml && a.magic_offset == b.magic_offset;
}

/// Run detection once and validate. Returns the result for the caller to
/// cross-check against describe_buffer.
DetectionResult detect_once(ByteSpan bytes, std::string_view name, std::size_t input_size) {
    DetectionHints hints;
    hints.file_name = name;

    DetectionResult out;
    const Status    status = uapp::files::detect_buffer(bytes, hints, out);

    // detector.h: "Returns Status::FileEmpty when the sample has no bytes at
    // all. Otherwise returns Status::Ok" — including for input we cannot
    // identify, which is an Ok outcome, not an error.
    if (input_size == 0) {
        ORACLE(status == Status::FileEmpty, "an empty buffer did not report FileEmpty");
        ORACLE(out.format == FileFormat::Unknown, "an empty buffer claimed a format");
        return out;
    }
    ORACLE(status == Status::Ok, "a non-empty buffer returned something other than Ok");

    check_detection_invariants(out, input_size, !name.empty());
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const ByteSpan bytes(data, size);

    // 1. The upload path proper: bytes, and nothing else.
    const DetectionResult anonymous = detect_once(bytes, std::string_view(), size);

    // 2. Determinism. detect_buffer is documented `noexcept` and pure; a
    //    differing second verdict on identical bytes means state leaked
    //    across calls or an uninitialised field was read. Cheap to check and
    //    the kind of defect that is nearly invisible in unit tests.
    const DetectionResult again = detect_once(bytes, std::string_view(), size);
    ORACLE(same_result(anonymous, again), "detect_buffer is not deterministic");

    // 3. The same bytes with a filename attached. The name is advisory only,
    //    so the byte-derived half of the verdict must come out identical to
    //    the anonymous run — that is detector.h §1 stated as a property
    //    rather than as a comment.
    const std::size_t   index = (size == 0) ? 0u : (static_cast<std::size_t>(data[0]) % kNameCount);
    const std::string_view name(kNames[index]);
    const DetectionResult named = detect_once(bytes, name, size);

    ORACLE(named.format == anonymous.format, "a filename changed the detected format");
    ORACLE(named.container_format == anonymous.container_format,
           "a filename changed the container format");
    ORACLE(named.magic_offset == anonymous.magic_offset, "a filename changed the magic offset");
    ORACLE(named.container_truncated == anonymous.container_truncated,
           "a filename changed the container-truncation verdict");
    // Confidence and method MAY legitimately differ: when the bytes matched
    // nothing, a hint moves them from None/NoMatch to ExtensionOnly/
    // ExtensionHintOnly. That is the only permitted divergence.
    if (anonymous.identified()) {
        ORACLE(named.confidence == anonymous.confidence,
               "a filename changed the confidence of a byte-identified format");
        ORACLE(named.method == anonymous.method,
               "a filename changed the method of a byte-identified format");
    }

    // 4. The full spec §8 File value over the same untrusted bytes: this is
    //    the checksum pass (every byte), the metadata derivation and the
    //    capability lookup, none of which detect_buffer touches. Same buffer,
    //    still no filesystem.
    DetectionHints describe_hints;
    describe_hints.file_name = name;
    File file;
    const Status describe_status = uapp::files::describe_buffer(
        bytes, describe_hints, FileId(), /*created_at_ms=*/0, /*modified_at_ms=*/0, file);

    if (size == 0) {
        ORACLE(describe_status == Status::FileEmpty, "describe_buffer accepted an empty buffer");
    } else {
        ORACLE(describe_status == Status::Ok, "describe_buffer rejected a non-empty buffer");
        // file.h: size is the file's size; mime_type is "the media type of
        // the DETECTED format, not of the claimed one".
        ORACLE(file.size == static_cast<std::uint64_t>(size), "File.size disagrees with the input");
        ORACLE(file.format == anonymous.format, "describe_buffer and detect_buffer disagree");
        ORACLE(file.mime_type == uapp::files::format_mime_type(file.format),
               "File.mime_type is not the detected format's media type");
        ORACLE(file.metadata.claimed_format == named.extension_hint,
               "File.metadata.claimed_format does not match the extension hint");
        // checksum.h shapes the digest as {algorithm, length, buffer}; a
        // length past the buffer would be a memory bug in any consumer.
        ORACLE(file.checksum.length <= uapp::files::kMaxChecksumBytes,
               "Checksum.length exceeds its own buffer");
    }

    return 0;
}
