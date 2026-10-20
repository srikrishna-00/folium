// SPDX-License-Identifier: Apache-2.0
//
// tools/loadtest/ocr_loadtest.cpp — scale and repeated-use harness for
// ocr/engine.
//
// WHY THIS MODULE IS SHAPED DIFFERENTLY FROM EVERY OTHER RUNNER HERE. OCR has
// no corpus and no row count. It takes one image and returns text. Feeding it
// "100 000 operations" would measure nothing the per-call latency did not
// already say. The two things that ARE real load questions for it are:
//
//   1. LATENCY vs IMAGE SIZE, up to the documented kMaxImagePixels ceiling of
//      20 Mpx. A phone camera at 12 MP lands inside that range, so the curve
//      between 24 kpx and 19 Mpx is the curve a scanner feature actually
//      walks. The run also steps one tile past the ceiling, because a module
//      whose limit is a crash and not a Status is a module that ships a
//      denial-of-service.
//
//   2. MEMORY ACROSS REPEATED CALLS. Tesseract is a C API behind a handle;
//      ocr/engine owns that handle and must not accumulate per-call state.
//      Repeated back-to-back recognition with CURRENT (not peak) RSS sampled
//      across it is the only way to see a leak, because a leak is a rising
//      floor and the peak would hide it behind the first large allocation.
//      This runner therefore does both the in-engine loop (same handle, many
//      images) and the open/close loop (many handles), since those two leak
//      in different places.
//
// THE IMAGE. tests/ocr/ocr_fixture.h holds a 300x80 Gray8 render of
// "HELLO WORLD" with known ground truth, generated from source and verified
// against the real tesseract(1) CLI. Tiling it is how this harness reaches 19
// megapixels without checking in a 19 MB binary and without inventing pixels
// that no one has ever confirmed are readable: every tile is text a human has
// verified Tesseract can read, so correctness stays checkable at every scale
// (recognised block count should track the tile count, and the text should
// still be the ground truth). Reaching into tests/ for a header-only,
// dependency-free constexpr array is a deliberate, narrow exception, recorded
// in this directory's CMakeLists.txt.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "loadtest_util.h"

#include "ocr_fixture.h"
#include "uapp/ocr/engine.h"
#include "uapp/status.h"

#ifndef UAPP_LOADTEST_TESSDATA
#define UAPP_LOADTEST_TESSDATA ""
#endif

namespace {

using uapp::Status;
using uapp::is_ok;
using uapp::loadtest::Timer;
using uapp::loadtest::current_rss_mib;
using uapp::loadtest::emit_row;
using uapp::loadtest::note;
using uapp::loadtest::peak_rss_mib;
using uapp::loadtest::status_text;

namespace fx = uapp::test::ocr;

constexpr const char kBuiltInTessdata[] = UAPP_LOADTEST_TESSDATA;

/// A Gray8 image made of tiles_x * tiles_y copies of the verified fixture.
struct TiledImage final {
    std::vector<std::uint8_t> pixels;
    int width  = 0;
    int height = 0;
    long long tiles = 0;
};

/// `filled` caps how many of the tiles_x*tiles_y slots actually get text; the
/// rest stay white. Passing -1 fills every slot. Partial filling is what makes
/// the `density` runner possible: same pixel count, different glyph count.
[[nodiscard]] TiledImage tile(int tiles_x, int tiles_y, long long filled = -1) {
    TiledImage img;
    img.width  = fx::kClearTextWidth * tiles_x;
    img.height = fx::kClearTextHeight * tiles_y;
    const long long slots =
        static_cast<long long>(tiles_x) * static_cast<long long>(tiles_y);
    img.tiles = (filled < 0 || filled > slots) ? slots : filled;
    const std::size_t w = static_cast<std::size_t>(img.width);
    const std::size_t h = static_cast<std::size_t>(img.height);
    img.pixels.assign(w * h, 255u);

    const std::size_t tile_w = static_cast<std::size_t>(fx::kClearTextWidth);
    const std::size_t tile_h = static_cast<std::size_t>(fx::kClearTextHeight);
    long long placed = 0;
    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            if (placed >= img.tiles) { break; }
            ++placed;
            const std::size_t ox = static_cast<std::size_t>(tx) * tile_w;
            const std::size_t oy = static_cast<std::size_t>(ty) * tile_h;
            for (std::size_t row = 0; row < tile_h; ++row) {
                std::memcpy(&img.pixels[(oy + row) * w + ox],
                            &fx::kClearTextPixels[row * tile_w], tile_w);
            }
        }
    }
    return img;
}

[[nodiscard]] uapp::ocr::ImageView view_of(const TiledImage& img) {
    uapp::ocr::ImageView v;
    v.pixels         = img.pixels.data();
    v.capacity_bytes = img.pixels.size();
    v.width          = img.width;
    v.height         = img.height;
    v.stride_bytes   = static_cast<std::size_t>(img.width);
    v.format         = uapp::ocr::PixelFormat::Gray8;
    return v;
}

/// Did the engine read the text it is supposed to read? Checked at every
/// scale, because a latency number for a recogniser that stopped recognising
/// is worse than no number.
[[nodiscard]] bool reads_ground_truth(const uapp::ocr::Result& r) {
    for (const uapp::ocr::TextBlock& b : r.blocks) {
        if (b.text.find("HELLO") != std::string::npos) { return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. latency vs image size
// ---------------------------------------------------------------------------

void run_sizes(const std::string& tessdata) {
    note("== ocr: image size ladder ==");

    // 24 000 px per tile. 28x28 == 18.8 Mpx, just inside kMaxImagePixels
    // (20 Mpx); 29x29 == 20.18 Mpx, just outside it.
    const int ladder[][2] = {{1, 1}, {3, 3}, {7, 7}, {14, 14}, {20, 20}, {28, 28}, {29, 29}};

    for (const auto& step : ladder) {
        const TiledImage img = tile(step[0], step[1]);
        const long long pixels =
            static_cast<long long>(img.width) * static_cast<long long>(img.height);
        const bool over = static_cast<std::size_t>(pixels) > uapp::ocr::kMaxImagePixels;

        uapp::ocr::Options options;
        options.tessdata_path = tessdata;
        options.language      = uapp::ocr::kLanguageEnglish;
        options.granularity   = uapp::ocr::Granularity::Paragraph;
        options.source_dpi    = 300;

        uapp::ocr::Engine engine;
        if (const Status s = engine.open(options); !is_ok(s)) {
            emit_row("ocr", "open", 0, 1, 0.0, "FAIL", status_text(s).c_str());
            return;
        }

        uapp::ocr::Result result;
        Timer t;
        const Status s  = engine.recognise(view_of(img), 0, result);
        const double ms = t.ms();

        char detail[256];
        if (over) {
            std::snprintf(detail, sizeof detail, "%s px=%lld dims=%dx%d",
                          status_text(s).c_str(), pixels, img.width, img.height);
            emit_row("ocr", "limit_max_image_pixels", pixels, 1, ms,
                     s == Status::OcrImageSizeInvalid ? "limit" : "FAIL", detail);
            continue;
        }
        if (!is_ok(s)) {
            std::snprintf(detail, sizeof detail, "%s px=%lld", status_text(s).c_str(), pixels);
            emit_row("ocr", "recognise", pixels, 1, ms, "FAIL", detail);
            continue;
        }
        const bool correct = reads_ground_truth(result);
        std::snprintf(detail, sizeof detail,
                      "dims=%dx%d tiles=%lld blocks=%zu mean_conf=%.1f Mpx_per_s=%.2f "
                      "reads_truth=%d",
                      img.width, img.height, img.tiles, result.blocks.size(),
                      static_cast<double>(result.mean_confidence),
                      ms > 0.0 ? (static_cast<double>(pixels) / 1e6) / (ms / 1000.0) : 0.0,
                      correct ? 1 : 0);
        emit_row("ocr", "recognise", pixels, 1, ms, correct ? "ok" : "FAIL", detail);
    }
}

// ---------------------------------------------------------------------------
// 1b. the same pixel count, varying text density
// ---------------------------------------------------------------------------

/// The size ladder shows latency rising faster than pixel count, but on its
/// own it cannot say WHICH input grew: the tiled image adds pixels and glyphs
/// together. This holds the canvas at a fixed 9.6 Mpx (roughly an A4 page at
/// 300 DPI, which is the size docs/performance-strategy.md §2.4 writes its
/// 2.5 s Tesseract budget against) and varies only how much of it carries
/// text. If latency tracks the glyph count rather than the canvas, the cost is
/// in the recogniser, not in the page-layout pass — and the budget has to be
/// read per page of TEXT, not per megapixel.
void run_density(const std::string& tessdata) {
    note("== ocr: density at a fixed 9.6 Mpx canvas ==");

    const long long filled[] = {1, 25, 100, 400};

    for (const long long f : filled) {
        const TiledImage img = tile(20, 20, f);
        const long long pixels =
            static_cast<long long>(img.width) * static_cast<long long>(img.height);

        uapp::ocr::Options options;
        options.tessdata_path = tessdata;
        options.language      = uapp::ocr::kLanguageEnglish;
        options.granularity   = uapp::ocr::Granularity::Paragraph;
        options.source_dpi    = 300;

        uapp::ocr::Engine engine;
        if (const Status s = engine.open(options); !is_ok(s)) {
            emit_row("ocr", "open", 0, 1, 0.0, "FAIL", status_text(s).c_str());
            return;
        }

        uapp::ocr::Result result;
        Timer t;
        const Status s  = engine.recognise(view_of(img), 0, result);
        const double ms = t.ms();
        if (!is_ok(s)) {
            emit_row("ocr", "density", f, 1, ms, "FAIL", status_text(s).c_str());
            continue;
        }
        char detail[256];
        std::snprintf(detail, sizeof detail,
                      "px=%lld text_tiles=%lld chars=%lld blocks=%zu reads_truth=%d", pixels,
                      img.tiles, img.tiles * 11, result.blocks.size(),
                      reads_ground_truth(result) ? 1 : 0);
        emit_row("ocr", "density_fixed_9.6Mpx", f, 1, ms, "ok", detail);
    }
}

// ---------------------------------------------------------------------------
// 2. repeated recognition on ONE engine handle — the leak question
// ---------------------------------------------------------------------------

void run_repeat(const std::string& tessdata, long long calls) {
    note("== ocr: repeated recognition on one handle (%lld calls) ==", calls);

    const TiledImage img = tile(3, 3);  // 216 kpx: big enough to be real, cheap enough to loop

    uapp::ocr::Options options;
    options.tessdata_path = tessdata;
    options.language      = uapp::ocr::kLanguageEnglish;
    options.granularity   = uapp::ocr::Granularity::Paragraph;
    options.source_dpi    = 300;

    uapp::ocr::Engine engine;
    if (const Status s = engine.open(options); !is_ok(s)) {
        emit_row("ocr", "open", 0, 1, 0.0, "FAIL", status_text(s).c_str());
        return;
    }

    // Warm up out of the measurement: the first call pays model load and
    // first-touch page faults, and counting that as growth would manufacture
    // a leak that is not there.
    constexpr long long kWarmup = 5;
    uapp::ocr::Result warm;
    for (long long i = 0; i < kWarmup; ++i) {
        if (const Status s = engine.recognise(view_of(img), 0, warm); !is_ok(s)) {
            emit_row("ocr", "repeat_warmup", kWarmup, i, 0.0, "FAIL", status_text(s).c_str());
            return;
        }
    }

    const double rss_base = current_rss_mib();
    double rss_quarter = rss_base;
    double rss_half    = rss_base;
    bool all_correct   = true;

    Timer t;
    long long done = 0;
    Status err     = Status::Ok;
    for (long long i = 0; i < calls; ++i) {
        uapp::ocr::Result result;
        err = engine.recognise(view_of(img), 0, result);
        if (!is_ok(err)) { break; }
        if (!reads_ground_truth(result)) { all_correct = false; }
        ++done;
        if (done == calls / 4) { rss_quarter = current_rss_mib(); }
        if (done == calls / 2) { rss_half = current_rss_mib(); }
    }
    const double ms       = t.ms();
    const double rss_end  = current_rss_mib();
    const double growth   = rss_end - rss_base;
    const double per_call = done > 0 ? (growth * 1024.0) / static_cast<double>(done) : 0.0;

    char detail[320];
    std::snprintf(detail, sizeof detail,
                  "rss_base=%.2f rss_25pct=%.2f rss_50pct=%.2f rss_end=%.2f growth_MiB=%.3f "
                  "growth_KiB_per_call=%.3f all_correct=%d",
                  rss_base, rss_quarter, rss_half, rss_end, growth, per_call,
                  all_correct ? 1 : 0);

    // A leak is a FLOOR that keeps rising. 32 KiB per call sustained over
    // hundreds of calls would be ~1 MiB per 32 pages, which on a Tier D device
    // doing a 200-page batch is a kill. Anything under a few KiB per call is
    // allocator noise, not accumulation.
    const char* verdict = !is_ok(err) ? "FAIL" : (per_call > 32.0 ? "slow" : "ok");
    emit_row("ocr", "repeat_same_handle", calls, done, ms, verdict,
             is_ok(err) ? detail : status_text(err).c_str());
}

// ---------------------------------------------------------------------------
// 3. repeated OPEN/CLOSE — a handle leak looks nothing like a buffer leak
// ---------------------------------------------------------------------------

void run_open_close(const std::string& tessdata, long long cycles) {
    note("== ocr: open/recognise/close cycles (%lld) ==", cycles);

    const TiledImage img = tile(1, 1);

    uapp::ocr::Options options;
    options.tessdata_path = tessdata;
    options.language      = uapp::ocr::kLanguageEnglish;
    options.granularity   = uapp::ocr::Granularity::Paragraph;
    options.source_dpi    = 300;

    // One warm-up cycle so the model file is in the page cache.
    {
        uapp::ocr::Engine warm;
        if (const Status s = warm.open(options); !is_ok(s)) {
            emit_row("ocr", "open", 0, 1, 0.0, "FAIL", status_text(s).c_str());
            return;
        }
        uapp::ocr::Result r;
        if (const Status s = warm.recognise(view_of(img), 0, r); !is_ok(s)) {
            emit_row("ocr", "open_close_warmup", 1, 0, 0.0, "FAIL", status_text(s).c_str());
            return;
        }
    }

    const double rss_base = current_rss_mib();
    Timer t;
    long long done = 0;
    Status err     = Status::Ok;
    for (long long i = 0; i < cycles; ++i) {
        uapp::ocr::Engine engine;
        err = engine.open(options);
        if (!is_ok(err)) { break; }
        uapp::ocr::Result r;
        err = engine.recognise(view_of(img), 0, r);
        if (!is_ok(err)) { break; }
        engine.close();
        ++done;
    }
    const double ms      = t.ms();
    const double rss_end = current_rss_mib();
    const double growth  = rss_end - rss_base;

    char detail[256];
    std::snprintf(detail, sizeof detail,
                  "rss_base=%.2f rss_end=%.2f growth_MiB=%.3f growth_KiB_per_cycle=%.2f",
                  rss_base, rss_end, growth,
                  done > 0 ? (growth * 1024.0) / static_cast<double>(done) : 0.0);
    emit_row("ocr", "open_recognise_close_cycles", cycles, done, ms,
             !is_ok(err) ? "FAIL" : (growth > 8.0 ? "slow" : "ok"),
             is_ok(err) ? detail : status_text(err).c_str());
}

void print_usage() {
    std::fputs(
        "uapp-loadtest-ocr — image-size ladder and repeated-use leak check for ocr/engine\n"
        "\n"
        "Usage:\n"
        "  uapp-loadtest-ocr [--tessdata <dir>] [--calls N] [--cycles N]\n"
        "                    <sizes|density|repeat|open-close|all>\n",
        stderr);
}

}  // namespace

int main(int argc, char** argv) {
    std::string tessdata = kBuiltInTessdata;
    long long calls      = 200;
    long long cycles     = 25;
    std::string what;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tessdata") == 0 && i + 1 < argc) {
            tessdata = argv[++i];
        } else if (std::strcmp(argv[i], "--calls") == 0 && i + 1 < argc) {
            calls = std::atoll(argv[++i]);
        } else if (std::strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            cycles = std::atoll(argv[++i]);
        } else if (argv[i][0] == '-') {
            print_usage();
            return 2;
        } else {
            what = argv[i];
        }
    }
    if (what.empty()) {
        print_usage();
        return 2;
    }
    if (tessdata.empty()) {
        std::fputs("uapp-loadtest-ocr: no tessdata directory (none found at configure time; "
                   "pass --tessdata <dir>)\n",
                   stderr);
        return 3;
    }

    uapp::loadtest::emit_csv_header();
    note("loadtest-ocr: tessdata = %s", tessdata.c_str());

    const bool all = what == "all";
    if (all || what == "sizes")      { run_sizes(tessdata); }
    if (all || what == "density")    { run_density(tessdata); }
    if (all || what == "repeat")     { run_repeat(tessdata, calls); }
    if (all || what == "open-close") { run_open_close(tessdata, cycles); }

    note("loadtest-ocr: done. process peak RSS = %.2f MiB", peak_rss_mib());
    return 0;
}
