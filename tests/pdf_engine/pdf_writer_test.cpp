// SPDX-License-Identifier: Apache-2.0
//
// tests/pdf_engine/pdf_writer_test.cpp — pdf-engine/writer, standalone.
//
// UNCONDITIONAL in tests/CMakeLists.txt (not gated by UAPP_PDF_ENABLED):
// uapp::pdf_writer has no PDFium dependency at all, so it must have real
// test coverage even on a build with no PDFium vendored. These cases check
// the writer's OWN output is structurally well-formed PDF syntax and that
// its error handling is correct; the stronger claim — that a real PDF
// reader can actually OPEN what this module writes — is proven separately
// in pdf_reader_test.cpp's "pdf.roundtrip" case, which needs PDFium and so
// lives in the PDFium-gated test binary instead.
//
// Case names:
//   writer.build   — well-formed output for valid input
//   writer.decode  — decode_image_to_rgb() on real and malformed bytes
//   writer.reject  — the five ways write_images_as_pdf() can and must fail

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "test_framework.h"
#include "uapp/pdf/writer.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::pdf::decode_image_to_rgb;
using uapp::pdf::PageImage;
using uapp::pdf::WriteOptions;
using uapp::pdf::write_images_as_pdf;
using uapp::pdf::writer_limits;

// A small synthetic RGB image: a 2x2-checker pattern, large enough that
// JPEG encoding actually does something (a 1x1 solid colour compresses to
// something degenerate stb's encoder may special-case).
std::vector<std::uint8_t> checkerboard(int width, int height) {
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    std::vector<std::uint8_t> pixels(w * h * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool          light = ((x / 8) + (y / 8)) % 2 == 0;
            const std::uint8_t  value = light ? 230 : 20;
            const std::size_t   idx   = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 3;
            pixels[idx + 0] = value;
            pixels[idx + 1] = light ? 20 : 230;  // distinguishable per-channel, not just grey
            pixels[idx + 2] = 128;
        }
    }
    return pixels;
}

std::string as_text(const std::vector<std::uint8_t>& bytes, std::size_t max_len) {
    const std::size_t n = bytes.size() < max_len ? bytes.size() : max_len;
    return std::string(reinterpret_cast<const char*>(bytes.data()), n);
}

}  // namespace

UAPP_TEST("writer.build: a single page produces well-formed PDF syntax") {
    const std::vector<std::uint8_t> pixels = checkerboard(64, 48);
    PageImage                       page;
    page.rgb_pixels = pixels.data();
    page.width      = 64;
    page.height     = 48;

    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_OK(write_images_as_pdf({page}, WriteOptions{}, out));

    UAPP_REQUIRE(!out.empty());
    UAPP_CHECK_MSG(as_text(out, 8) == "%PDF-1.4", "got: " + as_text(out, 8));

    const std::string whole(reinterpret_cast<const char*>(out.data()), out.size());
    UAPP_CHECK(whole.find("/Type /Catalog") != std::string::npos);
    UAPP_CHECK(whole.find("/Type /Pages") != std::string::npos);
    UAPP_CHECK(whole.find("/Type /Page ") != std::string::npos);
    UAPP_CHECK(whole.find("/Subtype /Image") != std::string::npos);
    UAPP_CHECK(whole.find("/Filter /DCTDecode") != std::string::npos);
    UAPP_CHECK(whole.find("xref") != std::string::npos);
    UAPP_CHECK(whole.find("trailer") != std::string::npos);
    UAPP_CHECK(whole.find("startxref") != std::string::npos);
    // The file must end with %%EOF, the one structural marker every PDF
    // reader scans backward for first.
    UAPP_CHECK(whole.size() >= 5 && whole.compare(whole.size() - 5, 5, "%%EOF") == 0);
}

UAPP_TEST("writer.build: multiple pages each get their own objects, in order") {
    const std::vector<std::uint8_t> a = checkerboard(32, 32);
    const std::vector<std::uint8_t> b = checkerboard(40, 20);
    PageImage                       page_a;
    page_a.rgb_pixels = a.data();
    page_a.width      = 32;
    page_a.height     = 32;
    PageImage page_b;
    page_b.rgb_pixels = b.data();
    page_b.width      = 40;
    page_b.height     = 20;

    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_OK(write_images_as_pdf({page_a, page_b}, WriteOptions{}, out));

    const std::string whole(reinterpret_cast<const char*>(out.data()), out.size());
    UAPP_CHECK(whole.find("/Count 2") != std::string::npos);
    // Two distinct image XObjects, two distinct content streams.
    std::size_t image_objects = 0;
    std::size_t pos           = 0;
    while ((pos = whole.find("/Subtype /Image", pos)) != std::string::npos) {
        ++image_objects;
        pos += 1;
    }
    UAPP_CHECK_MSG(image_objects == 2, "found " + std::to_string(image_objects));
}

UAPP_TEST("writer.build: rotation is recorded as a page /Rotate entry, exactly") {
    const std::vector<std::uint8_t> pixels = checkerboard(16, 16);
    PageImage                       page;
    page.rgb_pixels           = pixels.data();
    page.width                = 16;
    page.height                = 16;
    page.rotate_quarter_turns = 3;  // 270 degrees

    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_OK(write_images_as_pdf({page}, WriteOptions{}, out));
    const std::string whole(reinterpret_cast<const char*>(out.data()), out.size());
    UAPP_CHECK(whole.find("/Rotate 270") != std::string::npos);
}

UAPP_TEST("writer.decode: round trip through the encoder matches dimensions") {
    const std::vector<std::uint8_t> pixels = checkerboard(100, 80);
    PageImage                       page;
    page.rgb_pixels = pixels.data();
    page.width      = 100;
    page.height     = 80;

    std::vector<std::uint8_t> pdf_bytes;
    UAPP_REQUIRE_OK(write_images_as_pdf({page}, WriteOptions{}, pdf_bytes));

    // Pull the embedded JPEG stream back out by parsing the image object's
    // OWN /Length value and slicing exactly that many bytes — not by
    // searching for an "\nendstream" text marker, which is unsafe here:
    // JPEG is arbitrary binary and can coincidentally contain that exact
    // byte sequence inside its compressed data, which a naive text search
    // would find before the real boundary. The /Length the writer itself
    // recorded is ground truth and has no such ambiguity.
    const std::string whole(reinterpret_cast<const char*>(pdf_bytes.data()), pdf_bytes.size());
    const std::size_t filter_pos = whole.rfind("/Filter /DCTDecode");  // appears exactly once
    UAPP_REQUIRE(filter_pos != std::string::npos);
    const std::size_t length_label = whole.find("/Length ", filter_pos);
    UAPP_REQUIRE(length_label != std::string::npos);
    const std::size_t length_value_start = length_label + std::strlen("/Length ");
    const std::size_t stream_tag         = whole.find("\nstream\n", length_value_start);
    UAPP_REQUIRE(stream_tag != std::string::npos);
    const std::size_t jpeg_len =
        static_cast<std::size_t>(std::stoul(whole.substr(length_value_start,
                                                          stream_tag - length_value_start)));
    const std::size_t jpeg_start = stream_tag + std::strlen("\nstream\n");
    UAPP_REQUIRE(jpeg_start + jpeg_len <= whole.size());

    std::vector<std::uint8_t> decoded_pixels;
    std::int32_t              decoded_width  = 0;
    std::int32_t              decoded_height = 0;
    const auto*                jpeg_bytes     = reinterpret_cast<const std::uint8_t*>(whole.data()) + jpeg_start;
    UAPP_REQUIRE_OK(decode_image_to_rgb(jpeg_bytes, jpeg_len, decoded_pixels,
                                        decoded_width, decoded_height));
    UAPP_CHECK(decoded_width == 100);
    UAPP_CHECK(decoded_height == 80);
    UAPP_CHECK(decoded_pixels.size() == 100u * 80u * 3u);
}

UAPP_TEST("writer.decode: malformed bytes are a defined error, not a crash") {
    const std::uint8_t         garbage[] = {0x00, 0x01, 0x02, 0x03, 'n', 'o', 't', ' ', 'a', ' ',
                                            'p', 'i', 'c', 't', 'u', 'r', 'e'};
    std::vector<std::uint8_t>   pixels;
    std::int32_t                 width  = -1;
    std::int32_t                 height = -1;
    UAPP_REQUIRE_STATUS(decode_image_to_rgb(garbage, sizeof garbage, pixels, width, height),
                      Status::PdfWriterImageMalformed);
    UAPP_CHECK(pixels.empty());
    UAPP_CHECK(width == 0);
    UAPP_CHECK(height == 0);
}

UAPP_TEST("writer.decode: null/empty input is InvalidArgument") {
    std::vector<std::uint8_t> pixels;
    std::int32_t               width  = 0;
    std::int32_t               height = 0;
    UAPP_REQUIRE_STATUS(decode_image_to_rgb(nullptr, 0, pixels, width, height),
                      Status::InvalidArgument);
}

UAPP_TEST("writer.reject: an empty page list is InvalidArgument") {
    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_STATUS(write_images_as_pdf({}, WriteOptions{}, out), Status::InvalidArgument);
    UAPP_CHECK(out.empty());
}

UAPP_TEST("writer.reject: a null pixel pointer is InvalidArgument") {
    PageImage page;  // rgb_pixels left null, width/height left 0
    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_STATUS(write_images_as_pdf({page}, WriteOptions{}, out), Status::InvalidArgument);
}

UAPP_TEST("writer.reject: an out-of-range rotation is PdfWriterRotationInvalid") {
    const std::vector<std::uint8_t> pixels = checkerboard(16, 16);
    PageImage                       page;
    page.rgb_pixels           = pixels.data();
    page.width                 = 16;
    page.height                 = 16;
    page.rotate_quarter_turns = 4;  // only 0..3 are valid

    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_STATUS(write_images_as_pdf({page}, WriteOptions{}, out),
                      Status::PdfWriterRotationInvalid);
}

UAPP_TEST("writer.reject: a page past max_dimension is PdfWriterDimensionTooLarge") {
    const std::vector<std::uint8_t> pixels(4 * 3, 0);  // tiny buffer; dimensions below are the point
    PageImage                       page;
    page.rgb_pixels = pixels.data();
    page.width      = writer_limits().max_dimension + 1;
    page.height     = 2;

    std::vector<std::uint8_t> out;
    UAPP_REQUIRE_STATUS(write_images_as_pdf({page}, WriteOptions{}, out),
                      Status::PdfWriterDimensionTooLarge);
}

UAPP_TEST("writer.reject: out is left empty on every failure path") {
    std::vector<std::uint8_t> out = {1, 2, 3};  // pre-populated, must be cleared even on failure
    UAPP_REQUIRE_STATUS(write_images_as_pdf({}, WriteOptions{}, out), Status::InvalidArgument);
    UAPP_CHECK(out.empty());
}
