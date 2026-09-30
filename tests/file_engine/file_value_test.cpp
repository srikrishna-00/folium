// SPDX-License-Identifier: Apache-2.0
//
// The spec section 8 `File` value: its construction from a buffer and from a
// path, the checksum, and — the part most worth guarding — the capability
// claims derived from docs/file-format-support.md.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "common_fixtures.h"
#include "test_framework.h"
#include "uapp/file_engine/byte_span.h"
#include "uapp/file_engine/capabilities.h"
#include "uapp/file_engine/checksum.h"
#include "uapp/file_engine/detector.h"
#include "uapp/file_engine/file.h"
#include "uapp/object_id.h"
#include "uapp/status.h"
#include "zip_fixture.h"

namespace {

using uapp::Status;
using uapp::files::byte_span_of;
using uapp::files::Capability;
using uapp::files::CapabilitySet;
using uapp::files::ChecksumAlgorithm;
using uapp::files::DetectionHints;
using uapp::files::File;
using uapp::files::FileFormat;
using uapp::files::FileId;
using uapp::files::FileLocation;

FileId make_file_id(std::uint8_t tag) {
    uapp::ObjectIdBytes raw{};
    raw[0]  = 0xF1;
    raw[15] = tag;
    return FileId(raw);
}

/// Write `content` to `path`. Returns false if the write failed.
bool write_file(const std::string& path, std::string_view content) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { return false; }
    const std::size_t written =
        content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
    const bool ok = (written == content.size());
    std::fclose(f);
    return ok;
}

bool has(CapabilitySet set, Capability c) { return set.has(c); }

}  // namespace

// ===========================================================================
// Capability claims. Each of these encodes a line of file-format-support.md,
// and each would be a user-visible broken promise if it drifted.
// ===========================================================================

UAPP_TEST("caps: DOCX does NOT claim EDIT — the matrix marks DOCX edit as P2") {
    const CapabilitySet docx = uapp::files::format_capabilities(FileFormat::Docx);
    UAPP_CHECK(has(docx, Capability::View));
    UAPP_CHECK(has(docx, Capability::Convert));
    UAPP_CHECK(has(docx, Capability::Extract));
    UAPP_CHECK_MSG(!has(docx, Capability::Edit),
                   "file-format-support.md §2 marks DOCX EDIT as P2 (post-1.0)");
    UAPP_CHECK_MSG(!has(docx, Capability::Annotate), "§2 marks DOCX ANNOTATION as unsupported");

    // XLSX, by contrast, IS editable per the same table — so this is not a
    // blanket "OOXML is read-only" rule, it is the actual matrix.
    const CapabilitySet xlsx = uapp::files::format_capabilities(FileFormat::Xlsx);
    UAPP_CHECK(has(xlsx, Capability::Edit));
}

UAPP_TEST("caps: PPTX claims EXTRACT and nothing else — v1.0 is text extraction only") {
    const CapabilitySet pptx = uapp::files::format_capabilities(FileFormat::Pptx);
    UAPP_CHECK_MSG(has(pptx, Capability::Extract), "text + notes extraction for search");
    UAPP_CHECK_MSG(!has(pptx, Capability::View),
                   "§4 marks PPTX VIEW as ◐ P2 — full slide rendering is post-1.0");
    UAPP_CHECK(!has(pptx, Capability::Edit));
    UAPP_CHECK(!has(pptx, Capability::Convert));
    UAPP_CHECK(pptx.count() == 1);
}

UAPP_TEST("caps: NOTHING claims SIGN — signature validation is P2/TBD") {
    // file-format-support.md §5.1: "A half-implemented signature check is
    // worse than none." A capability flag lighting up a Sign affordance is
    // exactly that, so the flag must be dark across the entire registry —
    // PDF included, which is the one format where it would be plausible.
    for (std::size_t i = 0; i < uapp::files::format_count(); ++i) {
        const FileFormat f = uapp::files::format_at(i);
        UAPP_CHECK_MSG(!uapp::files::format_capabilities(f).has(Capability::Sign),
                       uapp::files::format_name(f));
    }
}

UAPP_TEST("caps: PDF is the only format claiming ENCRYPT") {
    for (std::size_t i = 0; i < uapp::files::format_count(); ++i) {
        const FileFormat f       = uapp::files::format_at(i);
        const bool       encrypt = uapp::files::format_capabilities(f).has(Capability::Encrypt);
        UAPP_CHECK_MSG(encrypt == (f == FileFormat::Pdf), uapp::files::format_name(f));
    }
    // Notably NOT ZIP: §8 marks AES-encrypted ZIP read-only.
    UAPP_CHECK(!uapp::files::format_capabilities(FileFormat::Zip).has(Capability::Encrypt));
}

UAPP_TEST("caps: JPEG does not claim COMPRESS because re-encoding is lossy") {
    UAPP_CHECK(uapp::files::format_capabilities(FileFormat::Png).has(Capability::Compress));
    UAPP_CHECK(uapp::files::format_capabilities(FileFormat::Pdf).has(Capability::Compress));
    UAPP_CHECK(!uapp::files::format_capabilities(FileFormat::Jpeg).has(Capability::Compress));
}

UAPP_TEST("caps: GIF has no EXPORT because the matrix marks GIF CREATE as unsupported") {
    const CapabilitySet gif = uapp::files::format_capabilities(FileFormat::Gif);
    UAPP_CHECK(has(gif, Capability::View));
    UAPP_CHECK(has(gif, Capability::Edit));     // ◐, single frame
    UAPP_CHECK(has(gif, Capability::Convert));
    UAPP_CHECK(!has(gif, Capability::Export));  // CREATE ✖
}

UAPP_TEST("caps: an unidentified file gets NO capabilities") {
    UAPP_CHECK(uapp::files::format_capabilities(FileFormat::Unknown).empty());
    // And so does every format whose detection is not implemented: we
    // cannot offer to do anything with a file we cannot identify.
    UAPP_CHECK(uapp::files::format_capabilities(FileFormat::Mp3).empty());
    UAPP_CHECK(uapp::files::format_capabilities(FileFormat::Rar).empty());
}

UAPP_TEST("caps: the bitmask round-trips and names are total") {
    const CapabilitySet pdf = uapp::files::format_capabilities(FileFormat::Pdf);
    UAPP_CHECK(CapabilitySet(pdf.bits()) == pdf);
    UAPP_CHECK(pdf.count() == 9);  // all ten minus SIGN
    for (std::uint32_t i = 0; i < uapp::files::kCapabilityCount; ++i) {
        UAPP_CHECK(uapp::files::capability_name(uapp::files::capability_at(i)) != nullptr);
    }
}

// ===========================================================================
// Checksum.
// ===========================================================================

UAPP_TEST("checksum: deterministic, order-sensitive, and streams identically") {
    const std::string a = "the quick brown fox";
    const std::string b = "the quick brown fox";
    const std::string c = "the quick brown fix";

    UAPP_CHECK(uapp::files::checksum_of(byte_span_of(a)) ==
               uapp::files::checksum_of(byte_span_of(b)));
    UAPP_CHECK(uapp::files::checksum_of(byte_span_of(a)) !=
               uapp::files::checksum_of(byte_span_of(c)));

    // Feeding it in pieces must give the same digest as one shot — this is
    // what describe_path relies on.
    uapp::files::Fnv1a64 streamed;
    streamed.update(byte_span_of(std::string_view(a).substr(0, 4)));
    streamed.update(byte_span_of(std::string_view(a).substr(4)));
    UAPP_CHECK(streamed.finish() == uapp::files::checksum_of(byte_span_of(a)));
}

UAPP_TEST("checksum: shape is algorithm-tagged and SHA-256-ready") {
    const uapp::files::Checksum ck = uapp::files::checksum_of(byte_span_of("x"));
    UAPP_CHECK(ck.algorithm == ChecksumAlgorithm::Fnv1a64);
    UAPP_CHECK(ck.length == 8);
    UAPP_CHECK(ck.to_hex().size() == 16);
    UAPP_CHECK(std::string_view(uapp::files::checksum_algorithm_name(ck.algorithm)) ==
               "fnv1a64");
    // Room for a 32-byte digest already, so adding SHA-256 later is a new
    // enumerator rather than a data migration.
    UAPP_CHECK(uapp::files::kMaxChecksumBytes == 32);

    const uapp::files::Checksum none;
    UAPP_CHECK(none.empty());
    UAPP_CHECK(none.to_hex().empty());
}

UAPP_TEST("checksum: an empty buffer hashes to the bare offset basis, avalanched") {
    // Not an error — hashing nothing is well-defined. It must simply differ
    // from hashing a NUL byte.
    const uapp::files::Checksum empty = uapp::files::checksum_of(uapp::files::ByteSpan());
    const uapp::files::Checksum nul   = uapp::files::checksum_of(byte_span_of(std::string(1, '\0')));
    UAPP_CHECK(empty.algorithm == ChecksumAlgorithm::Fnv1a64);
    UAPP_CHECK(empty != nul);
}

UAPP_TEST("checksum: the avalanche finaliser actually mixes") {
    // Plain FNV-1a differs in only a few low bits for inputs differing in
    // one trailing bit. The finaliser exists to fix that; if it were
    // dropped, this would fail.
    uapp::files::Fnv1a64 a;
    a.update(byte_span_of("aaaaaaaa"));
    uapp::files::Fnv1a64 b;
    b.update(byte_span_of("aaaaaaab"));
    const std::uint64_t x       = a.value() ^ b.value();
    std::uint32_t       changed = 0;
    for (int i = 0; i < 64; ++i) {
        if (((x >> i) & 1u) != 0) { ++changed; }
    }
    UAPP_CHECK_MSG(changed > 16, std::to_string(changed) + " of 64 bits changed");
}

// ===========================================================================
// describe_buffer: the full File value.
// ===========================================================================

UAPP_TEST("file: describe_buffer fills every spec section 8 field") {
    const std::string docx = uapp::test::make_docx();
    DetectionHints    hints;
    hints.file_name = "Q3 report.DOCX";

    File file;
    UAPP_REQUIRE_OK(uapp::files::describe_buffer(byte_span_of(docx), hints, make_file_id(7),
                                                 1700000000000LL, 1700000500000LL, file));

    UAPP_CHECK(file.id == make_file_id(7));
    UAPP_CHECK(file.name == "Q3 report.DOCX");
    UAPP_CHECK_MSG(file.extension == "docx", file.extension);  // lower-cased
    UAPP_CHECK(file.format == FileFormat::Docx);
    UAPP_CHECK(file.mime_type ==
               "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    UAPP_CHECK(file.size == docx.size());
    UAPP_CHECK(file.checksum.algorithm == ChecksumAlgorithm::Fnv1a64);
    UAPP_CHECK(file.created_at_ms == 1700000000000LL);
    UAPP_CHECK(file.modified_at_ms == 1700000500000LL);
    UAPP_CHECK(file.location.kind == FileLocation::Kind::MemoryBuffer);
    UAPP_CHECK(file.location.value.empty());
    UAPP_CHECK(file.metadata.extension_present);
    UAPP_CHECK(file.metadata.extension_matches_content);
    UAPP_CHECK(file.metadata.claimed_format == FileFormat::Docx);
    UAPP_CHECK(file.metadata.find("detection.method") == "zip_central_directory");
    UAPP_CHECK(file.metadata.find("container.format") == "zip");
    UAPP_CHECK(file.capabilities == uapp::files::format_capabilities(FileFormat::Docx));
}

UAPP_TEST("file: a renamed file records the conflict in its metadata") {
    const std::string zip = uapp::test::make_plain_zip();
    DetectionHints    hints;
    hints.file_name = "invoice.pdf";

    File file;
    UAPP_REQUIRE_OK(
        uapp::files::describe_buffer(byte_span_of(zip), hints, FileId(), 0, 0, file));

    UAPP_CHECK(file.format == FileFormat::Zip);
    UAPP_CHECK(file.mime_type == "application/zip");
    UAPP_CHECK(!file.metadata.extension_matches_content);
    UAPP_CHECK(file.metadata.claimed_format == FileFormat::Pdf);
    UAPP_CHECK(file.metadata.find("extension.conflict_claimed") == "pdf");
    // And the capabilities are the ZIP's, not the PDF's: no OCR, no
    // annotation on a file that merely claims to be a PDF.
    UAPP_CHECK(!file.capabilities.has(Capability::Ocr));
    UAPP_CHECK(!file.capabilities.has(Capability::Annotate));
}

UAPP_TEST("file: an unidentified buffer still produces a valid File with no capabilities") {
    const std::string opaque(64, '\x01');
    DetectionHints    hints;
    hints.file_name = "mystery.mp3";

    File file;
    UAPP_REQUIRE_OK(
        uapp::files::describe_buffer(byte_span_of(opaque), hints, FileId(), 0, 0, file));

    UAPP_CHECK(file.format == FileFormat::Unknown);
    UAPP_CHECK(file.mime_type == "application/octet-stream");
    UAPP_CHECK(file.capabilities.empty());
    UAPP_CHECK(file.size == 64);
    // The most useful thing we can say: there is no sniffer for what the
    // name claims. That is a different statement from "unknown".
    UAPP_CHECK(file.metadata.find("detection.unimplemented_for") == "mp3");
}

UAPP_TEST("file: describe_buffer on an empty buffer is FileEmpty") {
    DetectionHints hints;
    File           file;
    UAPP_REQUIRE_STATUS(
        uapp::files::describe_buffer(uapp::files::ByteSpan(), hints, FileId(), 0, 0, file),
        Status::FileEmpty);
}

// ===========================================================================
// Path-based detection and description.
// ===========================================================================

UAPP_TEST("file.path: detect_path and describe_path agree with the buffer path") {
    const uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());

    const std::string content = uapp::test::make_xlsx();
    const std::string path    = dir.file("budget.xlsx");
    UAPP_REQUIRE(write_file(path, content));

    uapp::files::DetectionResult detected;
    UAPP_REQUIRE_OK(uapp::files::detect_path(path, detected));
    UAPP_CHECK(detected.format == FileFormat::Xlsx);
    UAPP_CHECK(detected.method == uapp::files::DetectionMethod::ZipCentralDirectory);

    File file;
    UAPP_REQUIRE_OK(uapp::files::describe_path(path, make_file_id(3), file));
    UAPP_CHECK(file.format == FileFormat::Xlsx);
    UAPP_CHECK(file.name == "budget.xlsx");
    UAPP_CHECK(file.size == content.size());
    UAPP_CHECK(file.location.kind == FileLocation::Kind::LocalPath);
    UAPP_CHECK(file.location.value == path);
    UAPP_CHECK(file.modified_at_ms > 0);
    // The streamed digest must equal the one-shot digest of the same bytes.
    UAPP_CHECK(file.checksum == uapp::files::checksum_of(byte_span_of(content)));
}

UAPP_TEST("file.path: a file larger than the head window is still classified from its tail") {
    // A DOCX padded past kHeadSampleBytes, so the central directory is only
    // reachable through the tail read. Without the tail window this would
    // fall back to local headers and lose confidence.
    const uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());

    uapp::test::ZipBuilder z;
    z.add("[Content_Types].xml", "<Types/>");
    z.add("word/media/big.bin", std::string(uapp::files::kHeadSampleBytes + 4096, 'Z'));
    z.add("word/document.xml", "<w:document/>");
    const std::string content = z.finish();
    UAPP_REQUIRE(content.size() > uapp::files::kHeadSampleBytes);

    const std::string path = dir.file("big.docx");
    UAPP_REQUIRE(write_file(path, content));

    uapp::files::DetectionResult r;
    UAPP_REQUIRE_OK(uapp::files::detect_path(path, r));
    UAPP_CHECK_MSG(r.format == FileFormat::Docx, uapp::files::format_name(r.format));
    UAPP_CHECK(r.method == uapp::files::DetectionMethod::ZipCentralDirectory);
    UAPP_CHECK(!r.container_truncated);
}

UAPP_TEST("file.path: rejects bad paths without touching the filesystem") {
    uapp::files::DetectionResult r;
    UAPP_REQUIRE_STATUS(uapp::files::detect_path("", r), Status::FilePathInvalid);
    UAPP_REQUIRE_STATUS(uapp::files::detect_path(std::string_view("a\0b", 3), r),
                        Status::FilePathInvalid);
}

UAPP_TEST("file.path: a missing file and a directory are both FileOpenFailed") {
    const uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());

    uapp::files::DetectionResult r;
    UAPP_REQUIRE_STATUS(uapp::files::detect_path(dir.file("nope.pdf"), r),
                        Status::FileOpenFailed);
    // A directory opens fine with fopen on some platforms; reading it does
    // not. The S_ISREG check turns that into a clean status.
    UAPP_REQUIRE_STATUS(uapp::files::detect_path(dir.file(""), r), Status::FileOpenFailed);
}

UAPP_TEST("file.path: a zero-byte file is FileEmpty, not Unknown") {
    const uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string path = dir.file("empty.txt");
    UAPP_REQUIRE(write_file(path, ""));

    uapp::files::DetectionResult r;
    UAPP_REQUIRE_STATUS(uapp::files::detect_path(path, r), Status::FileEmpty);

    File file;
    UAPP_REQUIRE_STATUS(uapp::files::describe_path(path, FileId(), file), Status::FileEmpty);
}

UAPP_TEST("file.path: a one-byte file is handled, not rejected") {
    const uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string path = dir.file("tiny.dat");
    UAPP_REQUIRE(write_file(path, std::string(1, '\xFF')));

    uapp::files::DetectionResult r;
    UAPP_REQUIRE_OK(uapp::files::detect_path(path, r));
    UAPP_CHECK(r.format == FileFormat::Unknown);

    File file;
    UAPP_REQUIRE_OK(uapp::files::describe_path(path, FileId(), file));
    UAPP_CHECK(file.size == 1);
    UAPP_CHECK(file.capabilities.empty());
}

UAPP_TEST("file.location: kind names are total") {
    UAPP_CHECK(std::string_view(uapp::files::file_location_kind_name(
                   FileLocation::Kind::LocalPath)) == "local_path");
    UAPP_CHECK(std::string_view(uapp::files::file_location_kind_name(
                   FileLocation::Kind::PlatformUri)) == "platform_uri");
    UAPP_CHECK(uapp::files::file_location_kind_name(
                   static_cast<FileLocation::Kind>(42)) != nullptr);
}
