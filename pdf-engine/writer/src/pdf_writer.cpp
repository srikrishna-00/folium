// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/writer/src/pdf_writer.cpp — see the public header for the full
// design rationale. This file is PDFium-independent by construction: it
// never includes a PDFium header and never links uapp::pdfium (see this
// module's CMakeLists.txt) — it only ever emits PDF syntax.

#include "uapp/pdf/writer.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include <stb_image.h>
#include <stb_image_write.h>

namespace uapp::pdf {

namespace {

// Appends `bytes` as a PDF stream object's raw content, tracked by hand
// rather than through stb's callback machinery directly, so the caller
// controls exactly where in the output buffer it lands (needed to record
// the object's own byte offset for the xref table below).
void append(std::vector<std::uint8_t>& out, const char* text) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(text);
    out.insert(out.end(), begin, begin + std::strlen(text));
}

void append(std::vector<std::uint8_t>& out, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
}

// stbi_write_jpg_to_func's callback: appends the encoder's output straight
// into the std::vector passed as `context`.
void jpeg_write_callback(void* context, void* data, int size) {
    if (size <= 0) { return; }
    auto* buffer = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

// A 10-digit, zero-padded, NUL-free xref entry: "nnnnnnnnnn ggggg n \n" —
// PDF's xref table format demands EXACTLY this fixed 20-byte width per
// entry, offset left-padded with zeros, generation always 5 digits.
std::string xref_entry(std::size_t offset) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", offset);
    return std::string(buf);
}

}  // namespace

WriterLimits writer_limits() noexcept { return WriterLimits{}; }

Status write_images_as_pdf(const std::vector<PageImage>& pages, const WriteOptions& options,
                           std::vector<std::uint8_t>& out) {
    out.clear();

    const WriterLimits limits = writer_limits();

    if (pages.empty()) { return Status::InvalidArgument; }
    if (static_cast<std::int32_t>(pages.size()) > limits.max_pages) {
        return Status::PdfWriterPageCountExceeded;
    }

    std::size_t total_pixels = 0;
    for (const PageImage& page : pages) {
        if (page.rgb_pixels == nullptr || page.width <= 0 || page.height <= 0) {
            return Status::InvalidArgument;
        }
        if (page.rotate_quarter_turns < 0 || page.rotate_quarter_turns > 3) {
            return Status::PdfWriterRotationInvalid;
        }
        if (page.width > limits.max_dimension || page.height > limits.max_dimension) {
            return Status::PdfWriterDimensionTooLarge;
        }
        total_pixels += static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height);
    }
    if (total_pixels > limits.max_total_pixels) {
        return Status::PdfWriterTotalPixelsTooLarge;
    }

    std::vector<std::uint8_t> buffer;
    buffer.reserve(total_pixels / 4);  // a rough JPEG-compressed-size guess, just to avoid reallocs

    std::vector<std::size_t> object_offsets;  // index 0 unused, matches PDF's 1-based object numbers
    object_offsets.push_back(0);

    append(buffer, "%PDF-1.4\n");

    const std::int32_t page_count = static_cast<std::int32_t>(pages.size());
    const std::int32_t catalog_obj = 1;
    const std::int32_t pages_obj   = 2;
    // Page i (0-indexed) occupies three consecutive object numbers:
    // page dict, content stream, image XObject — see the header's own
    // numbering note.
    auto page_dict_obj    = [](std::int32_t i) { return 3 + i * 3 + 0; };
    auto content_obj      = [](std::int32_t i) { return 3 + i * 3 + 1; };
    auto image_obj         = [](std::int32_t i) { return 3 + i * 3 + 2; };
    const std::int32_t total_objects = 2 + page_count * 3;

    // Catalog (object 1).
    object_offsets.push_back(buffer.size());
    append(buffer, std::to_string(catalog_obj) + " 0 obj\n");
    append(buffer, "<< /Type /Catalog /Pages " + std::to_string(pages_obj) + " 0 R >>\nendobj\n");

    // Pages tree (object 2) — written after we know every page object's
    // number, which we already do (computed by formula above), so this can
    // come before the per-page objects without a second pass.
    {
        std::string kids = "[";
        for (std::int32_t i = 0; i < page_count; ++i) {
            kids += std::to_string(page_dict_obj(i)) + " 0 R ";
        }
        kids += "]";
        object_offsets.push_back(buffer.size());
        append(buffer, std::to_string(pages_obj) + " 0 obj\n");
        append(buffer, "<< /Type /Pages /Kids " + kids + " /Count " + std::to_string(page_count) +
                           " >>\nendobj\n");
    }
    // Reserve offset slots for the per-page objects; filled in below, in
    // object-number order, so a slot is never read before it's written.
    object_offsets.resize(static_cast<std::size_t>(total_objects) + 1, 0);

    for (std::int32_t i = 0; i < page_count; ++i) {
        const PageImage& page = pages[static_cast<std::size_t>(i)];

        const double width_pt  = static_cast<double>(page.width) / options.source_dpi * 72.0;
        const double height_pt = static_cast<double>(page.height) / options.source_dpi * 72.0;

        // JPEG-encode this page's pixels via stb_image_write's callback
        // form, capturing output directly into `jpeg`.
        std::vector<std::uint8_t> jpeg;
        const int ok = stbi_write_jpg_to_func(jpeg_write_callback, &jpeg, page.width, page.height,
                                              /*comp=*/3, page.rgb_pixels, options.jpeg_quality);
        if (ok == 0 || jpeg.empty()) {
            out.clear();
            return Status::PdfWriterEncodeFailed;
        }

        // Content stream: scale the unit square to the page size in points
        // and paint the image XObject once.
        char content_cmd[128];
        std::snprintf(content_cmd, sizeof content_cmd, "q %.4f 0 0 %.4f 0 0 cm /Im%d Do Q",
                      width_pt, height_pt, i);
        const std::size_t content_len = std::strlen(content_cmd);

        object_offsets[static_cast<std::size_t>(content_obj(i))] = buffer.size();
        append(buffer, std::to_string(content_obj(i)) + " 0 obj\n");
        append(buffer, "<< /Length " + std::to_string(content_len) + " >>\nstream\n");
        append(buffer, content_cmd);
        append(buffer, "\nendstream\nendobj\n");

        object_offsets[static_cast<std::size_t>(page_dict_obj(i))] = buffer.size();
        append(buffer, std::to_string(page_dict_obj(i)) + " 0 obj\n");
        char page_header[256];
        std::snprintf(page_header, sizeof page_header,
                      "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.4f %.4f] /Rotate %d "
                      "/Resources << /XObject << /Im%d %d 0 R >> >> /Contents %d 0 R >>\nendobj\n",
                      pages_obj, width_pt, height_pt, page.rotate_quarter_turns * 90, i,
                      image_obj(i), content_obj(i));
        append(buffer, page_header);

        object_offsets[static_cast<std::size_t>(image_obj(i))] = buffer.size();
        append(buffer, std::to_string(image_obj(i)) + " 0 obj\n");
        append(buffer, "<< /Type /XObject /Subtype /Image /Width " + std::to_string(page.width) +
                           " /Height " + std::to_string(page.height) +
                           " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " +
                           std::to_string(jpeg.size()) + " >>\nstream\n");
        buffer.insert(buffer.end(), jpeg.begin(), jpeg.end());
        append(buffer, "\nendstream\nendobj\n");
    }

    const std::size_t xref_offset = buffer.size();
    append(buffer, "xref\n0 " + std::to_string(total_objects + 1) + "\n");
    append(buffer, "0000000000 65535 f \n");
    for (std::int32_t obj = 1; obj <= total_objects; ++obj) {
        append(buffer, xref_entry(object_offsets[static_cast<std::size_t>(obj)]));
    }
    append(buffer, "trailer\n<< /Size " + std::to_string(total_objects + 1) + " /Root " +
                       std::to_string(catalog_obj) + " 0 R >>\n");
    append(buffer, "startxref\n" + std::to_string(xref_offset) + "\n%%EOF");

    out = std::move(buffer);
    return Status::Ok;
}

Status decode_image_to_rgb(const std::uint8_t* bytes, std::size_t len,
                           std::vector<std::uint8_t>& out_pixels, std::int32_t& out_width,
                           std::int32_t& out_height) {
    out_pixels.clear();
    out_width  = 0;
    out_height = 0;

    if (bytes == nullptr || len == 0) { return Status::InvalidArgument; }
    if (len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status::PdfWriterImageMalformed;
    }

    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    unsigned char* decoded =
        stbi_load_from_memory(bytes, static_cast<int>(len), &width, &height, &channels_in_file,
                              /*desired_channels=*/3);
    if (decoded == nullptr) { return Status::PdfWriterImageMalformed; }

    const WriterLimits limits = writer_limits();
    if (width <= 0 || height <= 0 || width > limits.max_dimension ||
        height > limits.max_dimension) {
        stbi_image_free(decoded);
        return Status::PdfWriterDimensionTooLarge;
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixel_count > limits.max_total_pixels) {
        stbi_image_free(decoded);
        return Status::PdfWriterTotalPixelsTooLarge;
    }

    out_pixels.assign(decoded, decoded + pixel_count * 3);
    stbi_image_free(decoded);
    out_width  = width;
    out_height = height;
    return Status::Ok;
}

}  // namespace uapp::pdf
