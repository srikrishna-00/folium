// SPDX-License-Identifier: Apache-2.0
//
// tests/ocr/ocr_engine_test.cpp — ocr::Engine against a real Tesseract.
//
// UAPP_OCR_TEST_TESSDATA is a compile definition from tests/CMakeLists.txt
// (sourced from cmake/UappTesseract.cmake's UAPP_TESSERACT_TESSDATA, found on
// THIS host only — never compiled into product code; see engine.h,
// Options::tessdata_path). If no tessdata directory was found at configure
// time, the macro is defined empty and the recognition cases ("ocr.read")
// are skipped with a clear message rather than failing for a reason that has
// nothing to do with this module.

#include <cstdint>
#include <cstring>
#include <vector>

#include "ocr_fixture.h"
#include "test_framework.h"
#include "uapp/ocr/engine.h"
#include "uapp/status.h"

using uapp::Status;
using uapp::ocr::Engine;
using uapp::ocr::ImageView;
using uapp::ocr::Options;
using uapp::ocr::PixelFormat;
using uapp::ocr::Result;

namespace {

#ifdef UAPP_OCR_TEST_TESSDATA
constexpr const char* kTessdataPath = UAPP_OCR_TEST_TESSDATA;
#else
constexpr const char* kTessdataPath = "";
#endif

[[nodiscard]] bool tessdata_available() noexcept { return kTessdataPath[0] != '\0'; }

[[nodiscard]] Status open_english(Engine& engine) {
    Options options;
    options.tessdata_path = kTessdataPath;
    return engine.open(options);
}

}  // namespace

// ===========================================================================
// ocr.read — real recognition against the embedded fixture image.
// ===========================================================================

UAPP_TEST("ocr.read recognises clear printed text with a real bounding box") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    UAPP_REQUIRE_OK(open_english(engine));

    ImageView image;
    image.pixels         = uapp::test::ocr::kClearTextPixels;
    image.capacity_bytes = sizeof(uapp::test::ocr::kClearTextPixels);
    image.width           = uapp::test::ocr::kClearTextWidth;
    image.height           = uapp::test::ocr::kClearTextHeight;
    image.stride_bytes    = 0;  // tightly packed
    image.format           = PixelFormat::Gray8;

    Result result;
    UAPP_REQUIRE_OK(engine.recognise(image, /*page=*/0, result));

    UAPP_REQUIRE_MSG(!result.blocks.empty(), "expected at least one recognised block");

    // Reassemble the recognised text across blocks (granularity defaults to
    // Paragraph, so this is ordinarily one block, but join defensively).
    std::string joined;
    for (const auto& block : result.blocks) {
        if (!joined.empty()) { joined += ' '; }
        joined += block.text;
    }
    // Tesseract's whitespace collapsing can vary slightly by version; check
    // for the two words rather than requiring byte-exact equality to the
    // ground truth string.
    UAPP_CHECK_MSG(joined.find("HELLO") != std::string::npos,
                  std::string("missing HELLO in: ") + joined);
    UAPP_CHECK_MSG(joined.find("WORLD") != std::string::npos,
                  std::string("missing WORLD in: ") + joined);

    const auto& block = result.blocks.front();
    UAPP_CHECK_MSG(!block.bounding_box.is_empty(), "bounding box must not be empty");
    UAPP_CHECK(block.bounding_box.left >= 0);
    UAPP_CHECK(block.bounding_box.top >= 0);
    UAPP_CHECK(block.bounding_box.right <= uapp::test::ocr::kClearTextWidth);
    UAPP_CHECK(block.bounding_box.bottom <= uapp::test::ocr::kClearTextHeight);
    UAPP_CHECK_MSG(block.confidence > 50.0f,
                  std::string("expected high confidence on clear text, got ") +
                      std::to_string(block.confidence));
    UAPP_CHECK(block.page == 0);
    UAPP_CHECK(!block.style.available);  // NOT IMPLEMENTED, honestly reported

    UAPP_CHECK_MSG(result.mean_confidence > 50.0f,
                  std::string("expected high mean confidence, got ") +
                      std::to_string(result.mean_confidence));
}

UAPP_TEST("ocr.read a word-granularity call returns multiple positioned blocks") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    Options options;
    options.tessdata_path = kTessdataPath;
    options.granularity    = uapp::ocr::Granularity::Word;
    UAPP_REQUIRE_OK(engine.open(options));

    ImageView image;
    image.pixels         = uapp::test::ocr::kClearTextPixels;
    image.capacity_bytes = sizeof(uapp::test::ocr::kClearTextPixels);
    image.width           = uapp::test::ocr::kClearTextWidth;
    image.height           = uapp::test::ocr::kClearTextHeight;
    image.format           = PixelFormat::Gray8;

    Result result;
    UAPP_REQUIRE_OK(engine.recognise(image, 0, result));
    // "HELLO" and "WORLD" are two separate words.
    UAPP_CHECK_MSG(result.blocks.size() >= 2,
                  std::string("expected >= 2 word blocks, got ") +
                      std::to_string(result.blocks.size()));
}

UAPP_TEST("ocr.read a blank image is Ok with zero or low-confidence blocks") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    UAPP_REQUIRE_OK(open_english(engine));

    std::vector<std::uint8_t> blank(200u * 100u, 255u);  // all white
    ImageView                 image;
    image.pixels         = blank.data();
    image.capacity_bytes = blank.size();
    image.width           = 200;
    image.height           = 100;
    image.format           = PixelFormat::Gray8;

    Result result;
    UAPP_REQUIRE_OK(engine.recognise(image, 0, result));
    // No text-bearing block should survive (blank-image blocks are dropped,
    // see engine.cpp): the result is OK with an empty or tiny block list.
    UAPP_CHECK_MSG(result.blocks.size() <= 1,
                  std::string("expected a near-empty result on a blank image, got ") +
                      std::to_string(result.blocks.size()) + " blocks");
}

UAPP_TEST("ocr.read static/noise does not crash and does not fabricate confidence") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    UAPP_REQUIRE_OK(open_english(engine));

    // Deterministic pseudo-noise (no <random> dependency, no flakiness).
    std::vector<std::uint8_t> noise(320u * 240u);
    std::uint32_t             state = 0x12345678u;
    for (auto& byte : noise) {
        state = state * 1664525u + 1013904223u;
        byte  = static_cast<std::uint8_t>(state >> 24);
    }

    ImageView image;
    image.pixels         = noise.data();
    image.capacity_bytes = noise.size();
    image.width           = 320;
    image.height           = 240;
    image.format           = PixelFormat::Gray8;

    Result result;
    // Must not crash. May legitimately return Ok (with junk low-confidence
    // blocks) or OcrRecognitionFailed; either is an acceptable, defined
    // outcome for meaningless input (engine.h design rule 6).
    const Status status = engine.recognise(image, 0, result);
    UAPP_CHECK(status == Status::Ok || status == Status::OcrRecognitionFailed);
    if (status == Status::Ok) {
        // Whatever confidence it reports, it must be the real Tesseract
        // number (bounded 0-100), not a fabricated high value.
        for (const auto& block : result.blocks) {
            UAPP_CHECK(block.confidence >= 0.0f && block.confidence <= 100.0f);
        }
    }
}

// ===========================================================================
// ocr.bounds / ocr.open — input validation, no real OCR needed.
// ===========================================================================

UAPP_TEST("ocr.bounds an oversized image is rejected before recognition") {
    ImageView image;
    std::uint8_t dummy = 0;
    image.pixels         = &dummy;
    image.capacity_bytes = 1;
    image.width           = 1;
    image.height           = 1;
    image.format           = PixelFormat::Gray8;

    // Dimension over the cap.
    image.width = uapp::ocr::kMaxImageDimension + 1;
    image.height = 10;
    UAPP_REQUIRE_STATUS(image.validate(), Status::OcrImageSizeInvalid);

    // Within per-dimension caps but over the total-pixel cap.
    image.width  = 20000;
    image.height = 1001;  // 20000 * 1001 > kMaxImagePixels (20,000,000)
    UAPP_REQUIRE_STATUS(image.validate(), Status::OcrImageSizeInvalid);
}

UAPP_TEST("ocr.bounds a too-small buffer is rejected, not read out of bounds") {
    std::uint8_t tiny[4] = {0, 0, 0, 0};
    ImageView    image;
    image.pixels         = tiny;
    image.capacity_bytes = sizeof(tiny);
    image.width           = 10;  // claims 10x10 Gray8 = 100 bytes, buffer has 4
    image.height           = 10;
    image.format           = PixelFormat::Gray8;
    UAPP_REQUIRE_STATUS(image.validate(), Status::OcrImageBufferTooSmall);
}

UAPP_TEST("ocr.bounds required_image_bytes matches validate()'s own arithmetic") {
    const std::size_t needed = uapp::ocr::required_image_bytes(100, 50, 0, PixelFormat::Gray8);
    UAPP_CHECK(needed == 100u * 50u);

    const std::size_t rgba = uapp::ocr::required_image_bytes(100, 50, 0, PixelFormat::Rgba8888);
    UAPP_CHECK(rgba == 100u * 50u * 4u);

    // A padded stride must be honoured, not ignored.
    const std::size_t padded = uapp::ocr::required_image_bytes(100, 50, 256, PixelFormat::Gray8);
    UAPP_CHECK(padded == 256u * 50u);

    // Unusable geometry is 0, always meaning "do not allocate this".
    UAPP_CHECK(uapp::ocr::required_image_bytes(0, 50, 0, PixelFormat::Gray8) == 0u);
    UAPP_CHECK(uapp::ocr::required_image_bytes(-1, 50, 0, PixelFormat::Gray8) == 0u);
    UAPP_CHECK(uapp::ocr::required_image_bytes(1000000, 1000000, 0, PixelFormat::Gray8) == 0u);
}

UAPP_TEST("ocr.open rejects a malformed or disallowed language tag") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    Options options;
    options.tessdata_path = kTessdataPath;

    options.language = "eng+deu";  // multi-language syntax: rejected outright
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrLanguageMalformed);

    options.language = "../etc";  // path-shaped nonsense
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrLanguageMalformed);

    options.language = "ENG";  // wrong case
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrLanguageMalformed);

    options.language = "deu";  // well-formed, but not vetted/vendored
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrLanguageNotImplemented);

    options.language = "";
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrLanguageMalformed);
}

UAPP_TEST("ocr.open rejects a bad tessdata path cleanly") {
    Engine engine;
    Options options;
    options.tessdata_path = "/no/such/directory/ever";
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrDataPathInvalid);

    options.tessdata_path = "";
    UAPP_REQUIRE_STATUS(engine.open(options), Status::OcrDataPathInvalid);
}

UAPP_TEST("ocr.open rejects out-of-range source_dpi and min_confidence") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    Options options;
    options.tessdata_path = kTessdataPath;

    options.source_dpi = uapp::ocr::kMinSourceDpi - 1;
    UAPP_REQUIRE_STATUS(engine.open(options), Status::InvalidArgument);

    options.source_dpi = uapp::ocr::kMaxSourceDpi + 1;
    UAPP_REQUIRE_STATUS(engine.open(options), Status::InvalidArgument);

    options.source_dpi    = 300;
    options.min_confidence = -1.0f;
    UAPP_REQUIRE_STATUS(engine.open(options), Status::InvalidArgument);

    options.min_confidence = 101.0f;
    UAPP_REQUIRE_STATUS(engine.open(options), Status::InvalidArgument);
}

UAPP_TEST("ocr.open recognise() on an unopened Engine is OcrNotOpen") {
    Engine    engine;
    ImageView image;
    std::uint8_t dummy = 0;
    image.pixels         = &dummy;
    image.capacity_bytes = 1;
    image.width           = 1;
    image.height           = 1;
    image.format           = PixelFormat::Gray8;

    Result result;
    UAPP_REQUIRE_STATUS(engine.recognise(image, 0, result), Status::OcrNotOpen);
    UAPP_CHECK(!engine.is_open());
}

UAPP_TEST("ocr.open close() is idempotent and safe on a default-constructed Engine") {
    Engine engine;
    engine.close();  // safe on a never-opened engine
    UAPP_CHECK(!engine.is_open());

    if (tessdata_available()) {
        UAPP_REQUIRE_OK(open_english(engine));
        UAPP_CHECK(engine.is_open());
        engine.close();
        UAPP_CHECK(!engine.is_open());
        engine.close();  // idempotent
        UAPP_CHECK(!engine.is_open());
    }
}

UAPP_TEST("ocr.open a negative page index is rejected") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    UAPP_REQUIRE_OK(open_english(engine));

    ImageView image;
    image.pixels         = uapp::test::ocr::kClearTextPixels;
    image.capacity_bytes = sizeof(uapp::test::ocr::kClearTextPixels);
    image.width           = uapp::test::ocr::kClearTextWidth;
    image.height           = uapp::test::ocr::kClearTextHeight;
    image.format           = PixelFormat::Gray8;

    Result result;
    UAPP_REQUIRE_STATUS(engine.recognise(image, -1, result), Status::InvalidArgument);
}

UAPP_TEST("ocr.open a null pixel pointer is rejected") {
    if (!tessdata_available()) {
        std::printf("  (skipped: no tessdata directory found at configure time)\n");
        return;
    }

    Engine engine;
    UAPP_REQUIRE_OK(open_english(engine));

    ImageView image;
    image.pixels         = nullptr;
    image.capacity_bytes = 100;
    image.width           = 10;
    image.height           = 10;
    image.format           = PixelFormat::Gray8;

    Result result;
    UAPP_REQUIRE_STATUS(engine.recognise(image, 0, result), Status::InvalidArgument);
}
