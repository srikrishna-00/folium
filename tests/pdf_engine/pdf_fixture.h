// SPDX-License-Identifier: Apache-2.0
//
// tests/pdf_engine/pdf_fixture.h — minimal, hand-built PDF files.
//
// WHY BUILT IN CODE RATHER THAN CHECKED IN AS A .pdf
// ---------------------------------------------------------------------------
// The same reasoning tests/file_engine/zip_fixture.h gives for ZIP:
//
//  1. A real-world PDF is an opaque blob. When a test fails against one, the
//     first question is always "is the file wrong or is the code wrong?", and
//     answering it means reading the blob in a hex editor. A fixture built
//     from source is readable, and a change to it is a reviewable diff.
//  2. A real-world PDF drags in fonts, ICC profiles and producer metadata of
//     unknown provenance and unknown licence. Spec section 35 wants a licence
//     answer for everything we distribute; "a PDF someone had lying around"
//     does not have one.
//  3. Determinism. No timestamps, no random object ids, no /ID array. The
//     same bytes every run, on every platform, so a render comparison can
//     eventually be exact.
//
// Nothing here uses a text-drawing operator, and that is deliberate: text
// needs a font, a font resource makes the fixture depend on PDFium's system
// font discovery, and system font discovery differs between a CI container
// and a laptop. Filled rectangles are resource-free and render identically
// everywhere, which is what a pixel assertion needs.
//
// WHAT THIS IS NOT: a corpus. Two valid pages and a few deliberately broken
// buffers exercise the contract; they do not explore the format. Spec
// section 29 puts the PDF parser first on the fuzz list and testing-strategy
// §6 says a parser without a fuzz target does not merge. There is no PDF
// fuzz target yet — see docs/changelog.md. These fixtures are the shape a
// seed corpus would start from, not a substitute for one.

#ifndef UAPP_TESTS_PDF_ENGINE_PDF_FIXTURE_H
#define UAPP_TESTS_PDF_ENGINE_PDF_FIXTURE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace uapp::test::pdf {

/// Geometry of the fixture, exposed so the assertions and the generator can
/// never disagree about it.
inline constexpr double kPage0WidthPoints  = 200.0;
inline constexpr double kPage0HeightPoints = 100.0;
inline constexpr double kPage1WidthPoints  = 300.0;
inline constexpr double kPage1HeightPoints = 400.0;
inline constexpr int    kFixturePageCount  = 2;

/// Page 0 draws one black rectangle covering the PDF-user-space box
/// [20,20]..[180,80] on a 200x100 page — i.e. the middle 80% horizontally
/// and middle 60% vertically. Assertions derive their sample points from
/// these, so changing the fixture cannot silently invalidate them.
inline constexpr double kRectLeftPoints   = 20.0;
inline constexpr double kRectBottomPoints = 20.0;
inline constexpr double kRectRightPoints  = 180.0;
inline constexpr double kRectTopPoints    = 80.0;

namespace detail {

inline void append(std::string& out, const char* text) { out.append(text); }

/// One 20-byte cross-reference entry. The width is mandated by ISO 32000-1
/// §7.5.4 ("each entry shall be exactly 20 bytes long, including the
/// end-of-line marker") and a PDF whose entries are 19 or 21 bytes is the
/// classic hand-written-PDF bug, so it is formatted once, here.
inline std::string xref_entry(std::size_t offset, bool in_use) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%010zu %05d %c \n", offset, in_use ? 0 : 65535,
                  in_use ? 'n' : 'f');
    return std::string(buffer);
}

inline std::string content_stream_object(int object_number, const std::string& stream) {
    std::string out;
    out += std::to_string(object_number);
    out += " 0 obj\n<< /Length ";
    out += std::to_string(stream.size());
    out += " >>\nstream\n";
    out += stream;
    out += "\nendstream\nendobj\n";
    return out;
}

}  // namespace detail

/// A complete, valid, two-page PDF 1.7 file.
///
/// Object layout — flat on purpose, so an offset in the xref table can be
/// checked by eye against the object it points at:
///   1 Catalog, 2 Pages, 3 Page[0], 4 Page[1], 5 Contents[0], 6 Contents[1].
inline std::vector<std::uint8_t> two_page_pdf() {
    // Page 0: fill the rectangle in black. `rg` is a DeviceRGB non-stroking
    // colour, `re` appends a rectangle, `f` fills it. No fonts, no external
    // resources, no transparency group — nothing that could resolve
    // differently on a different host.
    const std::string content0 =
        "0 0 0 rg\n20 20 160 60 re\nf\n";
    // Page 1 is intentionally blank: it proves page_count and per-page
    // MediaBox without the assertions being able to lean on page 0's marks.
    const std::string content1 = "% intentionally blank\n";

    std::vector<std::string> objects;
    objects.emplace_back("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    objects.emplace_back(
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n");
    objects.emplace_back(
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
        "/Resources << >> /Contents 5 0 R >>\nendobj\n");
    objects.emplace_back(
        "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
        "/Resources << >> /Contents 6 0 R >>\nendobj\n");
    objects.emplace_back(detail::content_stream_object(5, content0));
    objects.emplace_back(detail::content_stream_object(6, content1));

    std::string pdf;
    // The second line's high-bit bytes are the §7.5.2 convention that marks
    // the file as binary, so a tool that transfers it in text mode is
    // detectably wrong rather than subtly corrupting it.
    pdf += "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";

    std::vector<std::size_t> offsets;
    offsets.reserve(objects.size());
    for (const std::string& object : objects) {
        offsets.push_back(pdf.size());
        pdf += object;
    }

    const std::size_t xref_offset = pdf.size();
    pdf += "xref\n0 ";
    pdf += std::to_string(objects.size() + 1);
    pdf += "\n";
    pdf += detail::xref_entry(0, /*in_use=*/false);
    for (std::size_t offset : offsets) {
        pdf += detail::xref_entry(offset, /*in_use=*/true);
    }

    pdf += "trailer\n<< /Size ";
    pdf += std::to_string(objects.size() + 1);
    pdf += " /Root 1 0 R >>\nstartxref\n";
    pdf += std::to_string(xref_offset);
    pdf += "\n%%EOF\n";

    return std::vector<std::uint8_t>(pdf.begin(), pdf.end());
}

/// A PDF cut off part-way through. Keeps a valid header — so this is not the
/// same test as "random bytes" — and loses the xref table and trailer, which
/// is what a partial download or a truncated attachment actually looks like.
inline std::vector<std::uint8_t> truncated_pdf(std::size_t keep_bytes) {
    std::vector<std::uint8_t> full = two_page_pdf();
    if (keep_bytes >= full.size()) { keep_bytes = full.size() / 2; }
    full.resize(keep_bytes);
    return full;
}

/// Bytes that are not a PDF at all and do not claim to be.
inline std::vector<std::uint8_t> not_a_pdf() {
    const char* text = "This is a plain text file. It has never been a PDF.\n";
    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text),
                                     reinterpret_cast<const std::uint8_t*>(text) +
                                         std::char_traits<char>::length(text));
}

/// A `%PDF-` header with nothing behind it: the shape that gets past a magic
/// -number sniffer (file-engine/detection would call this a PDF) and must
/// still be refused by the parser. The interesting adversarial case, because
/// it is where detection and parsing are allowed to disagree.
inline std::vector<std::uint8_t> header_only_pdf() {
    const char* text = "%PDF-1.7\n";
    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text),
                                     reinterpret_cast<const std::uint8_t*>(text) +
                                         std::char_traits<char>::length(text));
}

/// A valid PDF whose startxref points far past the end of the buffer. Tests
/// the bounds path specifically rather than "is it broken somewhere".
inline std::vector<std::uint8_t> bad_startxref_pdf() {
    std::vector<std::uint8_t> pdf = two_page_pdf();
    std::string               text(pdf.begin(), pdf.end());
    const std::size_t         marker = text.rfind("startxref\n");
    if (marker == std::string::npos) { return pdf; }
    const std::size_t number_start = marker + std::char_traits<char>::length("startxref\n");
    const std::size_t number_end   = text.find('\n', number_start);
    if (number_end == std::string::npos) { return pdf; }
    text.replace(number_start, number_end - number_start, "999999999");
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace uapp::test::pdf

#endif  // UAPP_TESTS_PDF_ENGINE_PDF_FIXTURE_H
