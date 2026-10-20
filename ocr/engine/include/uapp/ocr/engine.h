// SPDX-License-Identifier: Apache-2.0
//
// ocr/engine/engine.h — printed-text OCR: one raster image in, positioned
// TextBlocks out.
//
// SCOPE, STATED BEFORE ANYTHING ELSE
// ---------------------------------------------------------------------------
// Spec section 13 lists nine things. This header implements TWO of them:
//
//     printed text, and the page-layout/paragraph structure Tesseract's own
//     page iterator reports over it.
//
// Everything else in section 13 — TABLES, FORMS, RECEIPTS, BUSINESS CARDS,
// HANDWRITING ("where feasible"), and MULTIPLE LANGUAGES beyond `eng` — is
// **NOT IMPLEMENTED**. Not stubbed, not partially present. The reasons are
// different for each and are worth separating, because three of them are
// "nobody has done the work" and two are "the work is not OCR work at all":
//
//   * **Tables** are not a recognition problem; they are a geometry problem
//     over a recognition result. Tesseract 5's own table detection is off by
//     default, is documented as experimental, and produces a cell grid only
//     for ruled tables. The honest shape for this is a separate layout pass
//     that consumes the `TextBlock` list below (which is why that list
//     carries real pixel coordinates), not a flag on this call.
//   * **Forms** are tables plus a field/label pairing model plus a schema.
//     Nothing in the project has a form schema yet.
//   * **Receipts and business cards** are not OCR features either: they are
//     *entity extraction* over OCR output (merchant, total, date / name,
//     title, phone). `ai/provider` is where that belongs — see
//     `docs/ai-architecture.md` §13.2 — and pointing it at this module's
//     output is a later integration, not a flag here.
//   * **Handwriting** needs a model we do not have. Tesseract's `eng`
//     `traineddata` is trained on printed text; the character error rate on
//     cursive is high enough that shipping it as "handwriting support" would
//     be a misrepresentation rather than a limitation. `tessdata` has no
//     handwriting model to vendor. This is the one item in section 13 that is
//     NOT IMPLEMENTED because the *dependency* cannot do it, and section 13
//     already hedges it with "where feasible".
//   * **Multiple languages**: the API below takes a language tag and
//     validates it, and exactly one value — `kLanguageEnglish` — is accepted
//     (`Status::OcrLanguageNotImplemented` for anything else). That is
//     deliberate and it is the difference between a one-language build and a
//     build that *pretends* language is not a parameter. Adding a language is
//     a vendoring task (a `tessdata_fast` pack, a licence line in
//     THIRD_PARTY_NOTICES, a size budget against `performance-strategy.md`
//     §1.2), not a code change here.
//
// Also NOT IMPLEMENTED, and called out because this schema exists to feed it:
// **searchable-PDF generation and document reconstruction**, which spec
// section 13's own last line names as the point of keeping coordinates. That
// is a `pdf-engine` writer feature (section 11's "OCR-to-PDF"), it needs the
// PDF *writing* dependency `docs/file-format-support.md` §7 assigns to QPDF,
// and nothing in the tree writes a PDF today. This module produces the input
// that feature will need and stops there.
//
// THE DEPENDENCY IS ALSO A BOOTSTRAP. `docs/spikes/SP-10-findings.md` vets
// Tesseract 5.5.3 and Leptonica 1.87.0; `third_party/tesseract/README.md`
// records that what this build links is a HOMEBREW INSTALL ON ONE DEV HOST,
// not a build we performed and not what would ship on Android or iOS. Two
// findings from that spike are load-bearing for anyone reading this header:
//
//   1. **The Homebrew libtesseract links libcurl and libarchive.** Verified
//      with `otool -L`. Tesseract 5 uses them to fetch and unpack
//      `traineddata` from a URL. A shipping build MUST be configured
//      `-DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON`: ADR-0002 decision 1 puts
//      document images behind client-side-only decryption, and an OCR
//      library with an HTTP client linked into it is a network surface on the
//      one code path that handles plaintext of sensitive documents. Nothing
//      in this module calls either, and nothing in this module passes
//      Tesseract a URL or a filename — but "we never call it" is not the same
//      property as "it is not there", and only the second one is checkable.
//   2. **Style/formatting detection is not available.** See `TextStyle`.
//
// DESIGN RULES THIS HEADER OBEYS, AND WHY
// ---------------------------------------------------------------------------
//  1. **The image buffer convention is `pdf-engine`'s, on purpose.**
//     `ImageView` is the read-only mirror of `pdf::RenderTarget`: same
//     `pixels` / `capacity_bytes` / `width` / `height` / `stride_bytes`
//     fields, same "0 stride means tightly packed" rule, same RGBA8888 byte
//     order, same `validate()` shape, same overflow-safe arithmetic. That is
//     not tidiness. The first real use of this module is "render a PDF page
//     with `pdf-engine`, then OCR the pixels", and the two conventions
//     matching means that pipeline is
//
//         pdf::RenderTarget t{buf, cap, w, h, stride};
//         doc.render_page(i, t);
//         ocr::ImageView v{buf, cap, w, h, stride, PixelFormat::Rgba8888};
//         engine.recognise(v, i, result);
//
//     with no conversion step and no second allocation. A different
//     convention here would have inserted a full-page copy into the one
//     pipeline the schema exists for. `ImageView` is a separate type rather
//     than a reuse of `RenderTarget` because this module must NOT depend on
//     `pdf-engine` (architecture.md section 4: engines depend on ports, never
//     on each other) and because the pointer is `const` here — OCR reads
//     pixels, it never writes them.
//
//  2. **Grayscale is a first-class input, not a conversion.** `PixelFormat::
//     Gray8` exists because the scanner pipeline's enhancement stage and
//     Tesseract itself both work in grayscale, and because 1 byte per pixel
//     is a 4x smaller peak footprint on the Tier D device floor that
//     `docs/adr/0002-phase0-signoff-decisions.md` decision 3 puts in scope
//     (2 GB RAM). Tesseract's `TessBaseAPISetImage` takes a
//     `bytes_per_pixel`, so both formats are a parameter rather than a
//     pre-pass; converting RGBA to gray ourselves would add a full-page
//     allocation to save nothing.
//
//  3. **Nothing in this module reads a file, and nothing takes a path except
//     the model directory.** No `recognise_path`, no `recognise_pdf`. Same
//     rule as `file-engine/detection` and `pdf-engine/reader`, for the same
//     ADR-0002-decision-1 reason: under E2EE a document image is decrypted
//     client-side into memory, and a path-taking OCR API forces a plaintext
//     temp file onto disk. The one path this module accepts is
//     `Options::tessdata_path`, which points at non-secret model data.
//
//  4. **No network, no server round-trip, ever.** This is a hard property of
//     the module, not a default: there is no host, no timeout, no retry and
//     no provider selection in this API, because ADR-0002 decision 1 means
//     the image this code sees has already been decrypted on the user's
//     device and may not leave it. `docs/ai-architecture.md` AIQ-8 (cloud OCR
//     offload with consent on the low-RAM tier) is an OPEN product question;
//     if it is ever answered yes, it belongs behind an `OCRProviderPort`
//     (architecture.md section 4 already names one) alongside this
//     implementation, never inside it.
//
//  5. **`uapp::Status` everywhere, `[[nodiscard]]` everywhere, no
//     exceptions.** ADR-0001. Note that this module links a C++ library
//     compiled WITH exceptions while this module is compiled with
//     `-fno-exceptions`; see `src/engine.cpp` for why that is sound here and
//     what it costs.
//
//  6. **Malformed input is expected input, and so is *meaningless* input.**
//     A hostile caller gets its arithmetic refused (rule 7). A user who
//     photographs a blank wall gets `Status::Ok` and an empty block list; a
//     user who photographs static gets `Status::Ok` and blocks whose
//     confidence is 14. Neither is an error, and inventing a status for them
//     would teach callers to read `Ok` as "found text". What callers get
//     instead is `Options::min_confidence` and `Result::mean_confidence`.
//
//  7. **Every bound is justified, not picked.** See `kMaxImageDimension`,
//     `kMaxImagePixels`, `kMaxTextBlocks` and `kMaxResultTextBytes` below.
//     Spec section 38 requires validated external input and checked integer
//     overflow; an OCR call on a 2^20 x 2^20 "image" is a one-line denial of
//     service otherwise.
//
// THREAD SAFETY
// ---------------------------------------------------------------------------
// An `Engine` is NOT thread-safe and must not be shared. Tesseract's
// `TessBaseAPI` holds the recognition state for the image currently set on
// it, so two threads calling `recognise()` on one `Engine` would interleave
// `SetImage` and `Recognize` and read each other's results. Separate
// `Engine` objects on separate threads are fine — Tesseract has no global
// init/destroy pair at all, which is the one way it is *simpler* than PDFium
// (contrast `pdf::Library`). `performance-strategy.md` §1.2 requires serial,
// one-page-at-a-time OCR on the device floor anyway, so the cheap answer and
// the required answer agree.

#ifndef UAPP_OCR_ENGINE_H
#define UAPP_OCR_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/status.h"

namespace uapp::ocr {

// ---------------------------------------------------------------------------
// Bounds (spec section 38)
// ---------------------------------------------------------------------------

/// Upper bound on an image dimension, in pixels, per side.
///
/// **Exactly `pdf::kMaxRenderDimension`, and that equality is the point.** A
/// PDF page rendered at the largest size `pdf-engine` will produce must be an
/// image this module will accept, or the pipeline rule 1 above exists for has
/// a hole in it that only shows up on the biggest input. If one of the two
/// constants ever moves, the other has to move with it.
///
/// Like PDFium's, this is an overflow guard rather than a tuning knob: it
/// keeps `width * 4` and `stride * height` far inside a `std::size_t` on
/// every supported platform, and it refuses a hostile `2^20 x 2^20` by
/// arithmetic we control before any multiplication happens.
inline constexpr int kMaxImageDimension = 20000;

/// Upper bound on total pixels in one `recognise()` call: 20 megapixels.
///
/// `kMaxImageDimension` alone is not enough. 20000 x 20000 is inside it and
/// is 400 Mpx — about 1.6 GB of RGBA input before Tesseract allocates
/// anything, on a device floor of 2 GB of TOTAL RAM
/// (`adr/0002-phase0-signoff-decisions.md` decision 3). A single-dimension
/// cap bounds the arithmetic; only an area cap bounds the memory.
///
/// Why 20 Mpx specifically, derived rather than chosen:
///
///   * **It must admit every page size the product promises.** At 300 DPI:
///     A4 is 2480 x 3508 = 8.7 Mpx, US Letter 2550 x 3300 = 8.4 Mpx, US
///     Legal 2550 x 4200 = 10.7 Mpx, A3 3508 x 4961 = 17.4 Mpx. All fit, A3
///     with the least room. A2 at 300 DPI (34.8 Mpx) does not, and is not a
///     page size this product claims to scan.
///   * **It must bound Tesseract's working set to something a 2 GB device
///     survives.** Tesseract does not recognise from the caller's buffer: it
///     copies into a Leptonica `Pix` (32 bpp), then thresholds to a 1 bpp
///     binary image, and keeps a grayscale copy for the LSTM recogniser —
///     roughly 4 + 4 + 1 + 0.125 bytes per pixel of *internal* allocation on
///     top of the caller's buffer. At 20 Mpx that is ~180 MB plus the
///     caller's 80 MB. That is ABOVE the 150 MB peak-RSS OCR target in
///     `performance-strategy.md` §1.1, and deliberately so: **this cap is a
///     refusal bound, not a budget.** It is the line past which we will not
///     even try; the Tier D *policy* ceiling is much lower (A4 at 200 DPI is
///     3.9 Mpx) and belongs to whichever scheduler picks the render DPI.
///     That tier-aware policy is NOT IMPLEMENTED here — this module has no
///     idea what device it is on, and guessing would be worse than refusing.
///
/// `kMaxImageDimension * kMaxImageDimension` is 4e8, which fits an
/// `std::size_t` on every supported platform, so the area is computed and
/// *then* compared rather than needing its own overflow dance.
inline constexpr std::size_t kMaxImagePixels = 20000000;

/// Upper bound on `TextBlock`s returned from one call.
///
/// Noise is the case this bounds, not text. A dense A4 page of 10 pt prose
/// holds roughly 500-800 words; a 20 Mpx image of random pixels can produce
/// far more "words" than that, because every speck becomes a candidate
/// component (measured: a 320 x 240 noise image yields 19 blocks at
/// granularity Word, which extrapolates past 50000 at the pixel cap). 20000
/// is ~25x the densest real page and still bounds the result vector. Past it,
/// `recognise()` returns `Status::OcrResultTooLarge` rather than truncating:
/// a silently truncated OCR result is indistinguishable from a complete one,
/// and the caller that would be hurt most is the searchable-PDF builder this
/// schema exists for.
inline constexpr std::size_t kMaxTextBlocks = 20000;

/// Upper bound on total recognised text bytes returned from one call, 4 MiB.
///
/// `kMaxTextBlocks` bounds the number of strings and not their size. A dense
/// A4 page is ~4 KB of text; 4 MiB is a thousand such pages' worth from one
/// image. Also `Status::OcrResultTooLarge`.
inline constexpr std::size_t kMaxResultTextBytes = 4u * 1024u * 1024u;

/// Longest accepted language tag, in bytes. Tesseract's tags are ISO 639-2/T
/// three-letter codes with optional script suffixes (`chi_sim`, `aze_cyrl`,
/// `kmr_latn`); 32 is well past the longest upstream publishes.
inline constexpr std::size_t kMaxLanguageTagBytes = 32;

/// The one language this build has a vetted, present model for.
///
/// A named constant rather than a bare `"eng"` at each call site, so that
/// "which languages does this build support?" has exactly one answer in the
/// source and `grep kLanguageEnglish` finds every place that has to change
/// when a second pack is vendored.
inline constexpr std::string_view kLanguageEnglish = "eng";

// ---------------------------------------------------------------------------
// Output schema — spec section 13's `TextBlock`, field for field
// ---------------------------------------------------------------------------

/// An axis-aligned box in **source-image pixel coordinates**, origin at the
/// TOP-LEFT of the image, `right`/`bottom` EXCLUSIVE.
///
/// Three conventions, each stated because each has a wrong answer that is
/// just as plausible:
///
///   * **Pixels, not points.** `pdf::PageSize` is deliberately in PDF user
///     space because a PDF page has an intrinsic size and no DPI; an OCR
///     input has no intrinsic size at all — it is a bitmap — so the only
///     coordinate system that exists here is the one the caller handed us.
///     Converting back to page points needs the DPI the caller rendered at,
///     which the caller knows and this module does not.
///   * **Top-left origin, y down.** This is Tesseract's own convention
///     (`TessPageIteratorBoundingBox` returns top/bottom with y increasing
///     downward) and the convention of every raster buffer in this project.
///     PDF user space is y-up; the conversion belongs in the eventual
///     searchable-PDF writer, where the page height is known.
///   * **Exclusive right/bottom**, so `width()` is `right - left` with no
///     off-by-one and an empty box is representable.
struct BoundingBox final {
    int left   = 0;
    int top    = 0;
    int right  = 0;
    int bottom = 0;

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    [[nodiscard]] bool is_empty() const noexcept { return right <= left || bottom <= top; }
};

/// Spec section 13's `style` field. **NOT IMPLEMENTED, and present anyway.**
///
/// Why it is here: section 13 names `style` in the schema, and a caller
/// written against a five-field `TextBlock` would have to change its type
/// when the sixth arrives. Why it is empty: Tesseract's LSTM recogniser does
/// not report font attributes at all. Measured on this machine against
/// 5.5.3 with `OEM_LSTM_ONLY`,
/// `TessResultIteratorWordFontAttributes()` returns a **null font name** and
/// leaves `is_bold`, `is_italic`, `is_underlined`, `is_monospace`,
/// `is_serif` and `is_smallcaps` at `false` for every word, including words
/// rendered from a bold bitmap font. The flags are populated only by the
/// deprecated `OEM_TESSERACT_ONLY` legacy engine, which needs the legacy
/// model data (absent from `tessdata_fast` — the variant
/// `performance-strategy.md` §1.2 requires us to ship) and which is both
/// slower and less accurate. Reporting `bold = false` from a field Tesseract
/// never set would be fabrication, so the struct carries the honesty flag
/// instead and nothing else.
///
/// What IS cheaply available and is deliberately not exposed: the
/// `pointsize` out-parameter of that same call *is* populated (measured: 31 /
/// 49 / 87 for the same glyphs rendered at 3x / 5x / 9x). It is not font
/// information — it is arithmetic over the text line's measured x-height and
/// the DPI we passed in — so exposing it as part of `style` would invite
/// exactly the misreading that it tells you something about the document's
/// typography. It is noted here as the identified starting point for whoever
/// implements this field, not as a half-measure to ship.
struct TextStyle final {
    /// Always `false` in this build. A caller must branch on this before
    /// reading anything else in this struct; there is nothing else to read.
    bool available = false;
};

/// Spec section 13's `TextBlock`, with every field populated from something
/// Tesseract actually reported.
struct TextBlock final {
    /// Recognised UTF-8 text. Never empty — blocks whose recognised text is
    /// empty or whitespace-only are dropped rather than returned, because
    /// Tesseract emits one for a blank image (measured: a blank page yields
    /// a single block with null text and confidence 0) and a "text block
    /// with no text" is noise in every consumer.
    ///
    /// Trailing newlines as Tesseract emits them are STRIPPED. Interior
    /// newlines are kept: at granularity `Paragraph` they are the line breaks
    /// within the paragraph, and they are real layout information.
    std::string text;

    /// Where it is, in source-image pixels. Always a non-empty box: Tesseract
    /// can report a bounding-box failure for a block, and such a block is
    /// dropped rather than returned with zeros.
    BoundingBox bounding_box;

    /// Tesseract's own confidence, **on its 0-100 scale, not normalised to
    /// 0-1**. Deliberately not rescaled: 0-100 is what Tesseract reports,
    /// what hOCR and TSV output carry, and what every published Tesseract
    /// confidence threshold is quoted in. Dividing by 100 here would mean
    /// every threshold a caller found in the literature had to be translated,
    /// and would imply a precision ("0.957") the number does not have.
    float confidence = 0.0f;

    /// The caller-supplied page index, passed through unchanged.
    ///
    /// This module OCRs ONE IMAGE per call and has no concept of a document,
    /// so it cannot know a page number — but spec section 13 puts `page` in
    /// the schema because the consumer is a multi-page searchable PDF. So the
    /// caller, which is iterating pages, supplies it and this module carries
    /// it. A module that invented a page number here (always 0? a counter?)
    /// would be guessing at its caller's loop.
    int page = 0;

    /// The language Tesseract reports it recognised this text in, e.g.
    /// "eng". Read back from `TessResultIteratorWordRecognitionLanguage()`
    /// rather than copied from `Options::language`, which are the same string
    /// today and would diverge the moment a multi-language pack
    /// (`"eng+deu"`) is wired: the point of the field is which language *won*
    /// per block. Empty if Tesseract did not report one.
    std::string language;

    /// NOT IMPLEMENTED. See `TextStyle`.
    TextStyle style;
};

/// What one `recognise()` call produced.
struct Result final {
    /// In Tesseract's reading order, which at granularity `Paragraph` or
    /// coarser is the layout analyser's own ordering of the page.
    std::vector<TextBlock> blocks;

    /// Tesseract's mean word confidence over the WHOLE page, 0-100,
    /// **including the blocks dropped as empty or filtered by
    /// `min_confidence`**. That is on purpose: it is the one number that
    /// answers "did this image contain legible text at all?", and
    /// recomputing it over the surviving blocks would make a page of static
    /// filtered down to two lucky words look like a confident result. 0 for a
    /// blank page.
    float mean_confidence = 0.0f;
};

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/// Pixel layouts this module accepts. Both are passed straight to Tesseract
/// as a `bytes_per_pixel`; neither is converted.
enum class PixelFormat : std::uint8_t {
    /// 8 bits per channel, byte order R, G, B, A at ascending addresses.
    /// **Byte-for-byte what `pdf::RenderTarget` produces**, which is the
    /// memory layout of Android's `ARGB_8888` and Skia's
    /// `kRGBA_8888_SkColorType` — not a 32-bit integer whose meaning depends
    /// on endianness. The alpha channel is ignored by recognition.
    Rgba8888 = 0,

    /// 8 bits per pixel, one channel, 0 = black. The scanner pipeline's
    /// enhancement output and the cheapest input on the device floor.
    Gray8 = 1,
};

/// A read-only, caller-owned source image.
///
/// The read-only mirror of `pdf::RenderTarget` — see design rule 1 in this
/// header's preamble for why the field set is identical rather than merely
/// similar. The caller owns the pixels and they must outlive the
/// `recognise()` call; they need not outlive it by any longer than that,
/// because Tesseract copies into its own `Pix` before recognising.
struct ImageView final {
    /// First byte of the first row. Must be non-null.
    const std::uint8_t* pixels = nullptr;

    /// Bytes addressable from `pixels`. Checked against the geometry; the
    /// check is the whole reason this field exists, so passing a made-up
    /// value here defeats it.
    std::size_t capacity_bytes = 0;

    /// Both must be in [1, kMaxImageDimension], and their product in
    /// [1, kMaxImagePixels].
    int width  = 0;
    int height = 0;

    /// Distance between the starts of consecutive rows, in bytes. 0 means
    /// "tightly packed", i.e. `width * bytes_per_pixel()`. Same rule as
    /// `pdf::RenderTarget::stride_bytes` and for the same reason: a caller
    /// handing us a platform bitmap passes that bitmap's own padded stride
    /// (`AndroidBitmapInfo::stride`, `CGBitmapContextGetBytesPerRow`), and
    /// refusing padded strides would force the copy this design exists to
    /// avoid.
    ///
    /// Must be >= `width * bytes_per_pixel()`. **Unlike `pdf::RenderTarget`,
    /// it need NOT be a multiple of 4.** That constraint was PDFium's — its
    /// 32 bpp bitmaps abort the process on an unaligned scanline — and
    /// Tesseract has no equivalent requirement: `TessBaseAPISetImage` takes
    /// `bytes_per_line` as an arbitrary count. Carrying a borrowed constraint
    /// would reject valid `Gray8` buffers for a reason that does not apply.
    std::size_t stride_bytes = 0;

    PixelFormat format = PixelFormat::Rgba8888;

    /// Bytes per pixel implied by `format`: 4 or 1.
    [[nodiscard]] std::size_t bytes_per_pixel() const noexcept;

    /// The stride actually in effect (resolves the 0 == packed case).
    /// Returns 0 if `width` is not usable, so a caller cannot get a
    /// plausible number out of a nonsense view.
    [[nodiscard]] std::size_t effective_stride() const noexcept;

    /// Validate the geometry against `capacity_bytes`, overflow-safely.
    /// Returns `Ok`, `InvalidArgument` (null `pixels`),
    /// `OcrImageSizeInvalid` or `OcrImageBufferTooSmall`. Public because a
    /// caller sizing a buffer pool wants the same answer without an `Engine`
    /// in hand — the same reason `pdf::RenderTarget::validate()` is public.
    [[nodiscard]] Status validate() const noexcept;
};

/// Minimum `capacity_bytes` for a given geometry, or 0 if the geometry is
/// unusable (non-positive, over `kMaxImageDimension`, over
/// `kMaxImagePixels`, stride below the row width, or arithmetically
/// overflowing). 0 always means "do not allocate this", never "no bytes
/// needed".
[[nodiscard]] std::size_t required_image_bytes(int width, int height, std::size_t stride_bytes,
                                               PixelFormat format) noexcept;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// What `TextBlock` corresponds to one unit of.
///
/// This is the knob that makes "page layout detection" and "paragraph
/// detection" (spec section 13) real features rather than claims: Tesseract's
/// page iterator already segments the page into blocks, paragraphs, text
/// lines and words during layout analysis, and all four levels carry their
/// own bounding box and confidence. Exposing the level is strictly more
/// honest than picking one and calling it "the" output.
enum class Granularity : std::uint8_t {
    /// One block per layout region the page analyser found. The coarsest
    /// "page layout detection" answer.
    Block = 0,

    /// One block per paragraph. **The default**, because it is the unit spec
    /// section 13 names explicitly ("paragraph detection") and the unit a
    /// document-reconstruction consumer wants.
    Paragraph = 1,

    /// One block per text line. What a searchable-PDF text layer usually
    /// wants, because a line is the coarsest unit whose box is still tight
    /// enough to position invisible text under the image.
    Line = 2,

    /// One block per word. The finest level this module exposes.
    ///
    /// Symbol-level (per-character) iteration exists in Tesseract and is NOT
    /// exposed: at 20 Mpx it would blow `kMaxTextBlocks` on a legitimate
    /// page, and no consumer in this project needs it.
    Word = 3,
};

/// How to open an `Engine`.
struct Options final {
    /// Directory holding `<language>.traineddata`. **Required — there is no
    /// default, deliberately.** A default would have to be either a compiled-
    /// in absolute path (`/opt/homebrew/share/tessdata` is this dev host's
    /// Homebrew prefix and belongs nowhere in product code) or Tesseract's
    /// own `TESSDATA_PREFIX` environment-variable fallback, which would make
    /// which model this app uses depend on the ambient environment of the
    /// process. On mobile this path is inside the app bundle and the platform
    /// layer is the only thing that knows it.
    ///
    /// Not copied or retained beyond the `open()` call.
    std::string_view tessdata_path;

    /// Which language to recognise. Only `kLanguageEnglish` is accepted; see
    /// this header's preamble and `Status::OcrLanguageNotImplemented`.
    ///
    /// Tesseract itself accepts a `+`-joined list (`"eng+deu"`). This module
    /// REJECTS that syntax (`Status::OcrLanguageMalformed`) rather than
    /// passing it through, because accepting it would mean a caller could ask
    /// for a language combination nothing in this build has vetted, measured
    /// or licensed, and would get either a silent single-language result or
    /// an init failure depending on what happened to be on disk.
    std::string_view language = kLanguageEnglish;

    /// What one `TextBlock` is.
    Granularity granularity = Granularity::Paragraph;

    /// Pixels per inch of the SOURCE image, for Tesseract's internal scale
    /// reasoning. Not a resampling request: nothing is resized. Tesseract
    /// warns (and guesses) when this is unset, and its guess is wrong for a
    /// cropped or upscaled image.
    ///
    /// 300 is the default because that is what `performance-strategy.md` §2.4
    /// quotes its OCR timings at; §1.2 drops the device floor to 200, which
    /// is the caller's decision to pass, not this module's to assume. Must be
    /// in [kMinSourceDpi, kMaxSourceDpi].
    int source_dpi = 300;

    /// Drop blocks whose confidence is below this (0-100 scale, as
    /// `TextBlock::confidence`). 0 keeps everything Tesseract reported.
    ///
    /// Default 0, i.e. no filtering, on purpose: a library that silently
    /// discards low-confidence text decides for its caller that a 40%-sure
    /// word is worthless, and for a full-text index it is not — a bad word in
    /// the index costs a false positive, while a missing word costs an
    /// undiscoverable document. The caller filters; `Result::mean_confidence`
    /// tells it whether it should.
    float min_confidence = 0.0f;
};

/// Bounds on `Options::source_dpi`. The low end is below any real scan; the
/// high end is past any real scanner, and both exist so that a nonsense value
/// is a `Status` rather than a number Tesseract reasons from.
inline constexpr int kMinSourceDpi = 36;
inline constexpr int kMaxSourceDpi = 2400;

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

/// One initialised recogniser, holding one loaded language model.
///
/// Open it once and call `recognise()` many times. The split exists because
/// loading `eng.traineddata` is the expensive part (tens of milliseconds and
/// the model's worth of resident memory), and `performance-strategy.md` §2.4
/// budgets 2.5 s for a page — a design that reloaded the model per page would
/// spend a measurable fraction of that budget on work it already did.
///
/// Move-only, like every other owning handle in the project. Not thread-safe;
/// see this header's preamble.
class Engine final {
public:
    Engine() noexcept = default;
    ~Engine();

    Engine(Engine&& other) noexcept;
    Engine& operator=(Engine&& other) noexcept;

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    /// Load the model and initialise the recogniser. Idempotent in the sense
    /// that it closes any previously opened engine first, so a failed
    /// `open()` never leaves a half-open `Engine` behind.
    ///
    /// Returns, and this list is a contract rather than a summary:
    ///   `Ok`
    ///   `OcrDataPathInvalid`        — path empty, over `PATH_MAX`, containing
    ///                                 a NUL byte, or not an existing
    ///                                 directory
    ///   `OcrLanguageMalformed`      — tag empty, over `kMaxLanguageTagBytes`,
    ///                                 or containing anything but [a-z_]
    ///                                 (which is what rejects `"eng+deu"`,
    ///                                 `"../etc"` and `"ENG"`)
    ///   `OcrLanguageNotImplemented` — a well-formed tag that is not
    ///                                 `kLanguageEnglish`
    ///   `OcrLanguageUnavailable`    — `<language>.traineddata` is not in that
    ///                                 directory
    ///   `OcrEngineInitFailed`       — every check above passed and Tesseract
    ///                                 still refused (a corrupt or
    ///                                 wrong-version model file)
    ///   `InvalidArgument`           — `source_dpi` or `min_confidence` out of
    ///                                 range
    [[nodiscard]] Status open(const Options& options);

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

    /// Recognise `image` and append nothing — `out` is CLEARED first, so a
    /// reused `Result` cannot accumulate a previous page's blocks.
    ///
    /// `page` is carried into every `TextBlock::page` unchanged and is not
    /// interpreted. Negative values are rejected (`InvalidArgument`) because
    /// no consumer of a page index has a meaning for one.
    ///
    /// Returns:
    ///   `Ok`                     — including for a blank or illegible image,
    ///                              which yields zero or low-confidence
    ///                              blocks. See design rule 6.
    ///   `OcrNotOpen`
    ///   `InvalidArgument`        — null `image.pixels`, or negative `page`
    ///   `OcrImageSizeInvalid`
    ///   `OcrImageBufferTooSmall`
    ///   `OcrRecognitionFailed`
    ///   `OcrResultTooLarge`
    [[nodiscard]] Status recognise(const ImageView& image, int page, Result& out) const;

    /// Release the model. Idempotent; safe on a default-constructed Engine.
    void close() noexcept;

private:
    /// `TessBaseAPI*`. A `void*` keeps Tesseract's headers out of this one,
    /// so nothing that includes it inherits a Tesseract include path — the
    /// same discipline `pdf::Document` applies to `FPDF_DOCUMENT`, and for a
    /// sharper reason here: `architecture.md` section 4 lists THREE OCR
    /// adapters behind one `OCRProviderPort` (Tesseract, Apple Vision, ML
    /// Kit), so the day the second one lands, "which OCR library is this?"
    /// must not already be baked into every caller's compile line.
    void* handle_ = nullptr;

    Granularity granularity_    = Granularity::Paragraph;
    int         source_dpi_     = 300;
    float       min_confidence_ = 0.0f;
};

}  // namespace uapp::ocr

#endif  // UAPP_OCR_ENGINE_H
