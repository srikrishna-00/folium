// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for file-engine/detection: signatures, the text heuristic, the
// ZIP-family disambiguation, malformed input, and the registry's
// "detection not implemented" query path.
//
// No filesystem here — these all run against in-memory buffers, which is
// also the mobile code path (detector.h section 2). Path-based detection and
// the File value are exercised in file_value_test.cpp.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "test_framework.h"
#include "uapp/file_engine/byte_span.h"
#include "uapp/file_engine/detector.h"
#include "uapp/file_engine/format.h"
#include "uapp/file_engine/text_probe.h"
#include "uapp/status.h"
#include "zip_fixture.h"

namespace {

using uapp::Status;
using uapp::files::ByteSpan;
using uapp::files::byte_span_of;
using uapp::files::DetectionConfidence;
using uapp::files::DetectionHints;
using uapp::files::DetectionMethod;
using uapp::files::DetectionResult;
using uapp::files::FileFormat;

/// Detect over a std::string, with an optional filename hint.
DetectionResult detect(std::string_view bytes, std::string_view name = std::string_view()) {
    DetectionHints hints;
    hints.file_name = name;
    DetectionResult out;
    const Status    s = uapp::files::detect_buffer(byte_span_of(bytes), hints, out);
    if (!uapp::is_ok(s)) { out = DetectionResult(); }
    return out;
}

std::string bytes_of(std::initializer_list<int> values) {
    std::string out;
    out.reserve(values.size());
    for (const int v : values) { out.push_back(static_cast<char>(v)); }
    return out;
}

}  // namespace

// ===========================================================================
// Signatures: one case per implemented format, from real byte signatures.
// ===========================================================================

UAPP_TEST("detect.magic: PNG from its 8-byte signature") {
    const std::string png = bytes_of({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}) +
                            std::string("\x00\x00\x00\x0D" "IHDR", 8);
    const DetectionResult r = detect(png, "photo.png");
    UAPP_CHECK(r.format == FileFormat::Png);
    UAPP_CHECK(r.confidence == DetectionConfidence::Magic);
    UAPP_CHECK(r.method == DetectionMethod::MagicSignature);
    UAPP_CHECK(std::string_view(r.mime_type()) == "image/png");
    UAPP_CHECK(!r.extension_conflicts);
}

UAPP_TEST("detect.magic: PNG signature must match all eight bytes") {
    // The first four bytes are right, the CRLF/EOF guard bytes are not.
    // A four-byte check would call this a PNG; the spec put those bytes
    // there precisely to catch it.
    const std::string almost = bytes_of({0x89, 'P', 'N', 'G', 0x0A, 0x0A, 0x1A, 0x0A, 0x00});
    UAPP_CHECK(detect(almost).format != FileFormat::Png);
}

UAPP_TEST("detect.magic: JPEG from SOI plus marker") {
    const std::string jpeg = bytes_of({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10}) + "JFIF";
    const DetectionResult r = detect(jpeg, "scan.jpg");
    UAPP_CHECK(r.format == FileFormat::Jpeg);
    UAPP_CHECK(r.confidence == DetectionConfidence::Magic);
    UAPP_CHECK(!r.extension_conflicts);
}

UAPP_TEST("detect.magic: GIF87a and GIF89a both detect as GIF") {
    UAPP_CHECK(detect("GIF87a\x01\x00\x01\x00").format == FileFormat::Gif);
    UAPP_CHECK(detect("GIF89a\x01\x00\x01\x00").format == FileFormat::Gif);
    // GIF8 alone is not enough.
    UAPP_CHECK(detect("GIF88a\x01\x00").format != FileFormat::Gif);
}

UAPP_TEST("detect.magic: PDF at offset zero") {
    const DetectionResult r = detect("%PDF-1.7\n1 0 obj\n", "report.pdf");
    UAPP_CHECK(r.format == FileFormat::Pdf);
    UAPP_CHECK(r.method == DetectionMethod::MagicSignature);
    UAPP_CHECK(r.magic_offset == 0);
    UAPP_CHECK(std::string_view(r.mime_type()) == "application/pdf");
}

UAPP_TEST("detect.magic: PDF with a leading preamble is found and the offset reported") {
    // Real-world PDFs sometimes carry junk before the header; every viewer
    // accepts them, so we do too — but we say where the header actually was
    // rather than normalising the fact away.
    const std::string junk(300, 'X');
    const DetectionResult r = detect(junk + "%PDF-1.4\n");
    UAPP_CHECK(r.format == FileFormat::Pdf);
    UAPP_CHECK(r.method == DetectionMethod::MagicSignatureAtOffset);
    UAPP_CHECK_MSG(r.magic_offset == 300, std::to_string(r.magic_offset));
}

UAPP_TEST("detect.magic: PDF header beyond the 1 KiB window is not accepted") {
    const std::string junk(2000, 'X');
    UAPP_CHECK(detect(junk + "%PDF-1.4\n").format != FileFormat::Pdf);
}

// ===========================================================================
// Plain text vs binary.
// ===========================================================================

UAPP_TEST("detect.text: ASCII is text") {
    const DetectionResult r = detect("hello, world\nsecond line\n", "notes.txt");
    UAPP_CHECK(r.format == FileFormat::PlainText);
    UAPP_CHECK(r.confidence == DetectionConfidence::Heuristic);
    UAPP_CHECK(r.method == DetectionMethod::TextHeuristic);
}

UAPP_TEST("detect.text: well-formed multi-byte UTF-8 is text") {
    // "naïve — 日本語 🎉": 2-, 3- and 4-byte sequences in one buffer.
    const DetectionResult r = detect("na\xC3\xAFve \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC"
                                     "\xE8\xAA\x9E \xF0\x9F\x8E\x89\n");
    UAPP_CHECK(r.format == FileFormat::PlainText);
}

UAPP_TEST("detect.text: a UTF-8 BOM settles it") {
    UAPP_CHECK(detect(bytes_of({0xEF, 0xBB, 0xBF}) + "x").format == FileFormat::PlainText);
    // UTF-16 is full of NUL bytes, so it can only be recognised by its BOM.
    UAPP_CHECK(detect(bytes_of({0xFF, 0xFE, 'h', 0x00, 'i', 0x00})).format ==
               FileFormat::PlainText);
}

UAPP_TEST("detect.text: a NUL byte means binary") {
    UAPP_CHECK(detect(std::string("abc\0def", 7)).format == FileFormat::Unknown);
}

UAPP_TEST("detect.text: invalid UTF-8 is not claimed as text") {
    // Overlong encoding of '/' — the classic path-traversal smuggling form.
    UAPP_CHECK(!uapp::files::probe_is_utf8_text(byte_span_of(bytes_of({0xC0, 0xAF})), true));
    // A UTF-16 surrogate half encoded as UTF-8 (CESU-8).
    UAPP_CHECK(
        !uapp::files::probe_is_utf8_text(byte_span_of(bytes_of({0xED, 0xA0, 0x80})), true));
    // Beyond U+10FFFF.
    UAPP_CHECK(!uapp::files::probe_is_utf8_text(
        byte_span_of(bytes_of({0xF4, 0x90, 0x80, 0x80})), true));
    // A stray continuation byte.
    UAPP_CHECK(!uapp::files::probe_is_utf8_text(byte_span_of(bytes_of({0x80})), true));
    // Windows-1252 'é' (0xE9) followed by ASCII: an incomplete 3-byte lead.
    UAPP_CHECK(!uapp::files::probe_is_utf8_text(byte_span_of(bytes_of({'c', 0xE9, 'a'})), true));
}

UAPP_TEST("detect.text: a sequence cut by the probe boundary is tolerated in a prefix only") {
    const std::string cut = bytes_of({'a', 0xE6, 0x97});  // 3-byte lead, one byte short
    UAPP_CHECK(uapp::files::probe_is_utf8_text(byte_span_of(cut), /*is_complete=*/false));
    UAPP_CHECK(!uapp::files::probe_is_utf8_text(byte_span_of(cut), /*is_complete=*/true));
}

// ===========================================================================
// The deliberate ambiguity: four ZIP-based formats and a plain ZIP.
// ===========================================================================

UAPP_TEST("detect.zip: a plain ZIP is reported as a ZIP, not guessed at") {
    const std::string    zip = uapp::test::make_plain_zip();
    const DetectionResult r  = detect(zip, "holiday.zip");
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK(r.container_format == FileFormat::Zip);
    UAPP_CHECK(r.method == DetectionMethod::ZipCentralDirectory);
    UAPP_CHECK(!r.unrecognised_ooxml);
    UAPP_CHECK(!r.container_truncated);
}

UAPP_TEST("detect.zip: DOCX, XLSX and PPTX are told apart from each other") {
    const DetectionResult docx = detect(uapp::test::make_docx(), "q3.docx");
    const DetectionResult xlsx = detect(uapp::test::make_xlsx(), "budget.xlsx");
    const DetectionResult pptx = detect(uapp::test::make_pptx(), "deck.pptx");

    UAPP_CHECK_MSG(docx.format == FileFormat::Docx,
                   uapp::files::format_name(docx.format));
    UAPP_CHECK_MSG(xlsx.format == FileFormat::Xlsx,
                   uapp::files::format_name(xlsx.format));
    UAPP_CHECK_MSG(pptx.format == FileFormat::Pptx,
                   uapp::files::format_name(pptx.format));

    // All three came from the authoritative central directory, so all three
    // get the strongest confidence available.
    UAPP_CHECK(docx.confidence == DetectionConfidence::MagicAndContainer);
    UAPP_CHECK(xlsx.confidence == DetectionConfidence::MagicAndContainer);
    UAPP_CHECK(pptx.confidence == DetectionConfidence::MagicAndContainer);
    UAPP_CHECK(docx.method == DetectionMethod::ZipCentralDirectory);

    // Each still knows it is a ZIP underneath.
    UAPP_CHECK(docx.container_format == FileFormat::Zip);
    UAPP_CHECK(pptx.container_format == FileFormat::Zip);
}

UAPP_TEST("detect.zip: ODT is read from its mimetype member, not from a part path") {
    const DetectionResult r = detect(uapp::test::make_odt(), "letter.odt");
    UAPP_CHECK_MSG(r.format == FileFormat::Odt, uapp::files::format_name(r.format));
    UAPP_CHECK(r.method == DetectionMethod::ZipMimetypeMember);
    UAPP_CHECK(r.confidence == DetectionConfidence::MagicAndContainer);
}

UAPP_TEST("detect.zip: an unrecognised OOXML package stays a ZIP and says so") {
    const DetectionResult r = detect(uapp::test::make_unknown_ooxml(), "diagram.vsdx");
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK_MSG(r.unrecognised_ooxml, "an OOXML package we cannot name must be flagged");
    // .vsdx is not in the registry at all, so there is no hint to conflict.
    UAPP_CHECK(r.extension_hint == FileFormat::Unknown);
}

UAPP_TEST("detect.zip: without a central directory, local headers still classify it") {
    // A streamed or partially-downloaded DOCX. The answer is the same, but
    // the confidence is lower and the truncation is reported, because a
    // local-header walk can be defeated by a crafted archive.
    uapp::test::ZipBuilder z;
    z.add("[Content_Types].xml", "<Types/>");
    z.add("word/document.xml", "<w:document/>");
    const DetectionResult r = detect(z.members_only(), "partial.docx");

    UAPP_CHECK_MSG(r.format == FileFormat::Docx, uapp::files::format_name(r.format));
    UAPP_CHECK(r.method == DetectionMethod::ZipLocalHeaders);
    UAPP_CHECK_MSG(r.confidence == DetectionConfidence::Magic,
                   "a local-header classification must NOT claim container confidence");
    UAPP_CHECK(r.container_truncated);
}

// ===========================================================================
// Never trust the extension alone (spec section 8).
// ===========================================================================

UAPP_TEST("detect.extension: a ZIP named .jpg is reported as a ZIP, with the conflict flagged") {
    const DetectionResult r = detect(uapp::test::make_plain_zip(), "vacation.jpg");
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK(r.extension_hint == FileFormat::Jpeg);
    UAPP_CHECK_MSG(r.extension_conflicts, "a renamed archive must be surfaced, not silently trusted");
}

UAPP_TEST("detect.extension: a .docx named .zip is not a conflict") {
    // A .docx IS a .zip. Reporting that as "this file is not what it claims"
    // would be a false alarm on a true statement.
    const DetectionResult r = detect(uapp::test::make_docx(), "package.zip");
    UAPP_CHECK(r.format == FileFormat::Docx);
    UAPP_CHECK(!r.extension_conflicts);
}

UAPP_TEST("detect.extension: a plain ZIP named .docx IS a conflict when we read the directory") {
    // The asymmetric half of the rule above. We walked the whole central
    // directory and there is no word/ part, so the .docx claim is false and
    // must be surfaced.
    const DetectionResult r = detect(uapp::test::make_plain_zip(), "payroll.docx");
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK(r.method == DetectionMethod::ZipCentralDirectory);
    UAPP_CHECK_MSG(r.extension_conflicts,
                   "a .docx with no word/ part, proven by a full directory walk, is a lie");
}

UAPP_TEST("detect.extension: an unreadable container named .docx is unproven, not accused") {
    // The same claim, but the directory was unreachable, so we never saw the
    // part that would have confirmed it. Accusing here would flag every
    // truncated download.
    const DetectionResult r = detect(bytes_of({'P', 'K', 0x03, 0x04}), "payroll.docx");
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK(r.container_truncated);
    UAPP_CHECK_MSG(!r.extension_conflicts, "unproven is not the same as false");
}

UAPP_TEST("detect.extension: the hint NEVER sets the format") {
    // Bytes that match nothing, with a name that claims PNG. If the
    // extension could promote a format, this would come back Png.
    const std::string     opaque = bytes_of({0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE});
    const DetectionResult r      = detect(opaque, "definitely.png");
    UAPP_CHECK_MSG(r.format == FileFormat::Unknown, uapp::files::format_name(r.format));
    UAPP_CHECK(r.extension_hint == FileFormat::Png);
    UAPP_CHECK(r.confidence == DetectionConfidence::ExtensionOnly);
    UAPP_CHECK(r.method == DetectionMethod::ExtensionHintOnly);
    // "the name is the only evidence" is not "the name and the bytes
    // disagree" — there was no second opinion to disagree with.
    UAPP_CHECK(!r.extension_conflicts);
}

UAPP_TEST("detect.extension: a .py source file detecting as text is not a conflict") {
    const DetectionResult r = detect("import sys\nprint(sys.argv)\n", "tool.py");
    UAPP_CHECK(r.format == FileFormat::PlainText);
    UAPP_CHECK(r.extension_hint == FileFormat::SourceCode);
    UAPP_CHECK(!r.extension_conflicts);
}

UAPP_TEST("detect.extension: parsing of odd filenames") {
    using uapp::files::extension_of;
    UAPP_CHECK(extension_of("report.PDF") == "pdf");            // case-folded
    UAPP_CHECK(extension_of(".bashrc").empty());                // a dotfile has no extension
    UAPP_CHECK(extension_of("noextension").empty());
    UAPP_CHECK(extension_of("trailing.").empty());
    UAPP_CHECK(extension_of("archive.tar.gz") == "gz");         // last component wins
    UAPP_CHECK(extension_of("/some.dir/file").empty());         // dot in a directory only
    UAPP_CHECK(extension_of("/a/b/c.txt") == "txt");
    UAPP_CHECK(extension_of("C:\\docs\\x.docx") == "docx");     // Windows separators
    UAPP_CHECK(extension_of("").empty());
    UAPP_CHECK(extension_of("x." + std::string(40, 'a')).empty());  // not an extension
}

// ===========================================================================
// Malformed, truncated and hostile input (spec section 28/29).
// ===========================================================================

UAPP_TEST("detect.malformed: an empty buffer is FileEmpty, not 'unknown'") {
    DetectionHints  hints;
    DetectionResult out;
    UAPP_REQUIRE_STATUS(uapp::files::detect_buffer(ByteSpan(), hints, out), Status::FileEmpty);
    // Distinguishing "nothing here" from "something unrecognised" is the
    // whole point: a re-index needs to tell a failed download from an
    // unsupported format.
}

UAPP_TEST("detect.malformed: a null pointer with a nonzero length is empty, not a crash") {
    DetectionHints  hints;
    DetectionResult out;
    UAPP_REQUIRE_STATUS(uapp::files::detect_buffer(ByteSpan(nullptr, 4096), hints, out),
                        Status::FileEmpty);
}

UAPP_TEST("detect.malformed: a one-byte buffer never reads past its end") {
    // Each of these is checked against signatures 3 to 8 bytes long. Under
    // ASan a single out-of-bounds compare fails the run.
    for (int b = 0; b < 256; ++b) {
        const std::string     one(1, static_cast<char>(b));
        const DetectionResult r = detect(one);
        // Printable ASCII is legitimately a one-character text file; the
        // rest must come back Unknown. Nothing may come back as PNG/ZIP/PDF,
        // whose signatures cannot fit in one byte.
        const bool text_like = (b >= 0x20 && b < 0x7F) || b == 0x09 || b == 0x0A ||
                               b == 0x0B || b == 0x0C || b == 0x0D;
        const FileFormat want = text_like ? FileFormat::PlainText : FileFormat::Unknown;
        UAPP_CHECK_MSG(r.format == want,
                       "byte " + std::to_string(b) + " -> " +
                           uapp::files::format_name(r.format));
    }
}

UAPP_TEST("detect.malformed: two bytes of a PNG signature is not a PNG") {
    const DetectionResult r = detect(bytes_of({0x89, 'P'}), "truncated.png");
    UAPP_CHECK(r.format == FileFormat::Unknown);
    UAPP_CHECK(r.extension_hint == FileFormat::Png);
}

UAPP_TEST("detect.malformed: a truncated ZIP central directory degrades, it does not lie") {
    // Take a real DOCX and cut it mid-central-directory: the EOCD is gone,
    // so the authoritative list is unreachable.
    const std::string full = uapp::test::make_docx();
    UAPP_REQUIRE(full.size() > 40);
    const std::string cut = full.substr(0, full.size() - 30);

    const DetectionResult r = detect(cut, "damaged.docx");
    // We still know it is a ZIP — the local headers survived — and the
    // local-header walk can still see word/document.xml. What must NOT
    // happen is a confident claim.
    UAPP_CHECK(r.container_format == FileFormat::Zip);
    UAPP_CHECK_MSG(r.container_truncated, "a missing EOCD must be reported");
    UAPP_CHECK_MSG(r.confidence != DetectionConfidence::MagicAndContainer,
                   "a truncated container must not yield the strongest confidence");
}

UAPP_TEST("detect.malformed: a bare ZIP signature with no headers at all") {
    // Four bytes that say "archive" and nothing behind them.
    const DetectionResult r = detect(bytes_of({'P', 'K', 0x03, 0x04}));
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK(r.confidence == DetectionConfidence::Magic);
    // The signature is all we had: there were no members to enumerate.
    UAPP_CHECK(r.method == DetectionMethod::MagicSignature);
    UAPP_CHECK(r.container_truncated);
}

UAPP_TEST("detect.malformed: an EOCD pointing past the end of the file is rejected") {
    // A crafted archive claiming its central directory lives at 0x7FFFFFFF.
    // Following that offset is the bug this test exists to prevent.
    std::string z = uapp::test::make_docx();
    UAPP_REQUIRE(z.size() > 22);
    const std::size_t eocd = z.size() - 22;
    // Overwrite the 4-byte central-directory offset at EOCD+16.
    z[eocd + 16] = static_cast<char>(0xFF);
    z[eocd + 17] = static_cast<char>(0xFF);
    z[eocd + 18] = static_cast<char>(0xFF);
    z[eocd + 19] = static_cast<char>(0x7F);

    const DetectionResult r = detect(z, "hostile.docx");
    UAPP_CHECK(r.container_format == FileFormat::Zip);
    UAPP_CHECK_MSG(r.container_truncated,
                   "an out-of-range CD offset must be treated as unreachable");
    UAPP_CHECK(r.confidence != DetectionConfidence::MagicAndContainer);
}

UAPP_TEST("detect.malformed: a central directory claiming 65534 entries terminates") {
    // The entry count is attacker-controlled. The walk must stop when it
    // runs off the end of the real directory rather than iterating 65534
    // times over whatever happens to follow.
    std::string z = uapp::test::make_plain_zip();
    UAPP_REQUIRE(z.size() > 22);
    const std::size_t eocd = z.size() - 22;
    z[eocd + 8]  = static_cast<char>(0xFE);  // entries on this disk
    z[eocd + 9]  = static_cast<char>(0xFF);
    z[eocd + 10] = static_cast<char>(0xFE);  // entries total
    z[eocd + 11] = static_cast<char>(0xFF);

    const DetectionResult r = detect(z, "bomb.zip");
    UAPP_CHECK(r.format == FileFormat::Zip);
    UAPP_CHECK(r.method == DetectionMethod::ZipCentralDirectory);
    UAPP_CHECK(!r.container_truncated);
}

UAPP_TEST("detect.malformed: a Zip64 sentinel falls back instead of following 0xFFFFFFFF") {
    // Zip64 directory parsing is NOT IMPLEMENTED. The sentinel must be
    // recognised as such — following it as a literal offset is the bug.
    std::string z = uapp::test::make_docx();
    UAPP_REQUIRE(z.size() > 22);
    const std::size_t eocd = z.size() - 22;
    for (std::size_t i = 16; i < 20; ++i) { z[eocd + i] = static_cast<char>(0xFF); }

    const DetectionResult r = detect(z, "huge.docx");
    UAPP_CHECK(r.container_format == FileFormat::Zip);
    UAPP_CHECK(r.container_truncated);
    UAPP_CHECK(r.confidence != DetectionConfidence::MagicAndContainer);
}

UAPP_TEST("detect.malformed: a member name length longer than the file") {
    // Local header claims a 60000-byte member name in a 40-byte file.
    uapp::test::ZipBuilder z;
    z.add("a.txt", "x");
    std::string data = z.members_only();
    UAPP_REQUIRE(data.size() > 30);
    data[26] = static_cast<char>(0x60);  // name length low byte
    data[27] = static_cast<char>(0xEA);  // name length high byte

    const DetectionResult r = detect(data, "liar.zip");
    UAPP_CHECK(r.format == FileFormat::Zip);  // still obviously a ZIP
    UAPP_CHECK(r.container_truncated);
}

UAPP_TEST("detect.malformed: high-entropy noise is Unknown, never a lucky match") {
    // A deterministic LCG, so a failure is reproducible.
    std::uint32_t state = 0x12345678u;
    std::string   noise;
    noise.reserve(4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        noise.push_back(static_cast<char>((state >> 16) & 0xFFu));
    }
    const DetectionResult r = detect(noise);
    UAPP_CHECK_MSG(r.format == FileFormat::Unknown, uapp::files::format_name(r.format));
    UAPP_CHECK(r.confidence == DetectionConfidence::None);
    UAPP_CHECK(std::string_view(r.mime_type()) == "application/octet-stream");
}

// ===========================================================================
// ByteSpan: the bounds-safety primitive, tested directly.
// ===========================================================================

UAPP_TEST("detect.span: no accessor can read out of range") {
    const std::uint8_t raw[4] = {1, 2, 3, 4};
    const ByteSpan     s(raw, 4);

    UAPP_CHECK(!s.covers(0, 5));
    UAPP_CHECK(!s.covers(4, 1));
    UAPP_CHECK(s.covers(4, 0));
    UAPP_CHECK(!s.covers(5, 0));
    // offset + length must not be allowed to wrap.
    UAPP_CHECK(!s.covers(2, static_cast<std::size_t>(-1)));

    UAPP_CHECK(s.at_or(3, 0xFF) == 4);
    UAPP_CHECK(s.at_or(4, 0xFF) == 0xFF);
    UAPP_CHECK(s.at_or(static_cast<std::size_t>(-1), 0xFF) == 0xFF);

    const std::uint8_t pattern[5] = {1, 2, 3, 4, 5};
    UAPP_CHECK(!s.starts_with(pattern, 5));
    UAPP_CHECK(s.starts_with(pattern, 4));
    UAPP_CHECK(!s.equal_at(1, pattern, 4));
    UAPP_CHECK(!s.equal_at(99, pattern, 1));

    std::uint32_t v32 = 0xDEADBEEFu;
    UAPP_CHECK(!s.read_u32le(1, v32));
    UAPP_CHECK_MSG(v32 == 0xDEADBEEFu, "a failed read must not half-write its output");
    UAPP_CHECK(s.read_u32le(0, v32));
    UAPP_CHECK(v32 == 0x04030201u);

    UAPP_CHECK(s.subspan(10, 4).empty());
    UAPP_CHECK(s.subspan(2, 100).size() == 2);
    UAPP_CHECK(s.text_at(0, 99).empty());
}

// ===========================================================================
// The registry: "not implemented" is a distinct, queryable answer.
// ===========================================================================

UAPP_TEST("registry.notimpl: an unimplemented format is distinguishable from no match") {
    // Bytes of a real MP3 frame header. We have no MP3 sniffer, so the
    // result is Unknown — but the registry can say *why*, which "unknown"
    // alone cannot.
    const DetectionResult r = detect(bytes_of({0xFF, 0xFB, 0x90, 0x00}), "song.mp3");
    UAPP_CHECK(r.format == FileFormat::Unknown);
    UAPP_CHECK(r.extension_hint == FileFormat::Mp3);

    UAPP_CHECK(!uapp::files::format_detection_is_implemented(FileFormat::Mp3));
    const char* why = uapp::files::format_detection_not_implemented_reason(FileFormat::Mp3);
    UAPP_REQUIRE(why != nullptr);
    UAPP_CHECK_MSG(std::string_view(why).find("NOT IMPLEMENTED") == 0, why);
}

UAPP_TEST("registry.notimpl: every implemented format reports no reason, and vice versa") {
    const FileFormat implemented[] = {
        FileFormat::PlainText, FileFormat::Pdf,  FileFormat::Png,  FileFormat::Jpeg,
        FileFormat::Gif,       FileFormat::Zip,  FileFormat::Docx, FileFormat::Xlsx,
        FileFormat::Pptx,      FileFormat::Odt,
    };
    for (const FileFormat f : implemented) {
        UAPP_CHECK_MSG(uapp::files::format_detection_is_implemented(f),
                       uapp::files::format_name(f));
        UAPP_CHECK_MSG(uapp::files::format_detection_not_implemented_reason(f) == nullptr,
                       uapp::files::format_name(f));
    }

    // And the converse across the WHOLE registry: the two queries can never
    // disagree, and no unimplemented format may be left without a reason.
    for (std::size_t i = 0; i < uapp::files::format_count(); ++i) {
        const FileFormat f    = uapp::files::format_at(i);
        const char*      why  = uapp::files::format_detection_not_implemented_reason(f);
        const bool       done = uapp::files::format_detection_is_implemented(f);
        UAPP_CHECK_MSG(done == (why == nullptr), uapp::files::format_name(f));
        if (!done) {
            UAPP_REQUIRE(why != nullptr);
            UAPP_CHECK_MSG(std::string_view(why).size() > 40,
                           std::string("reason too thin for ") + uapp::files::format_name(f));
        }
    }
}

UAPP_TEST("registry.notimpl: Unknown is a sentinel, not an unimplemented format") {
    UAPP_CHECK(!uapp::files::format_detection_is_implemented(FileFormat::Unknown));
    const char* why =
        uapp::files::format_detection_not_implemented_reason(FileFormat::Unknown);
    UAPP_REQUIRE(why != nullptr);
    UAPP_CHECK_MSG(std::string_view(why).find("NOT A FORMAT") == 0, why);
}

UAPP_TEST("registry: names, mime types and categories are total") {
    for (std::size_t i = 0; i < uapp::files::format_count(); ++i) {
        const FileFormat f = uapp::files::format_at(i);
        UAPP_CHECK(uapp::files::format_name(f) != nullptr);
        UAPP_CHECK(uapp::files::format_mime_type(f) != nullptr);
        UAPP_CHECK(uapp::files::format_primary_extension(f) != nullptr);
        UAPP_CHECK(std::string_view(uapp::files::format_name(f)) != "unknown" ||
                   f == FileFormat::Unknown);
    }
    // A value never assigned to any enumerator must not crash anything.
    const auto bogus = static_cast<FileFormat>(99999);
    UAPP_CHECK(std::string_view(uapp::files::format_name(bogus)) == "unknown");
    UAPP_CHECK(std::string_view(uapp::files::format_mime_type(bogus)) ==
               "application/octet-stream");
    UAPP_CHECK(uapp::files::format_capabilities(bogus).empty());
}

UAPP_TEST("registry: extension lookup is case-insensitive and dot-tolerant") {
    using uapp::files::format_from_extension;
    UAPP_CHECK(format_from_extension("PDF") == FileFormat::Pdf);
    UAPP_CHECK(format_from_extension(".pdf") == FileFormat::Pdf);
    UAPP_CHECK(format_from_extension("jpeg") == FileFormat::Jpeg);
    UAPP_CHECK(format_from_extension("jpg") == FileFormat::Jpeg);
    UAPP_CHECK(format_from_extension("") == FileFormat::Unknown);
    UAPP_CHECK(format_from_extension(".") == FileFormat::Unknown);
    UAPP_CHECK(format_from_extension("nosuchthing") == FileFormat::Unknown);
    UAPP_CHECK(format_from_extension(std::string(500, 'a')) == FileFormat::Unknown);
}
