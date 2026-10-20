// SPDX-License-Identifier: Apache-2.0
//
// ocr/engine/engine.cpp — the only translation unit in the project that
// includes a Tesseract header.
//
// WHY THE C API AND NOT tesseract::TessBaseAPI DIRECTLY
// ---------------------------------------------------------------------------
// <tesseract/capi.h> rather than <tesseract/baseapi.h>, even though this is
// C++ and the C++ class is right there. Three reasons, in order of weight:
//
//  1. **-fno-exceptions.** ADR-0001 compiles every C++ translation unit in
//     this project without exceptions; libtesseract is compiled WITH them.
//     That combination is link-compatible — the flag changes what *this*
//     object file may do, not what the library may do — but it is only SOUND
//     if no Tesseract exception is ever allowed to propagate into a frame
//     compiled this way, because such a frame has no unwind tables and the
//     result is `std::terminate` at best and undefined behaviour at worst.
//     The C API is the boundary that makes that checkable: every function in
//     capi.h is `extern "C"` and upstream catches internally rather than
//     throwing across it. Calling the C++ class would put a `throw` from
//     deep inside the recogniser one frame away from a `-fno-exceptions`
//     caller, with nothing but hope in between.
//
//     THIS IS A FLAGGED RESIDUAL RISK, not a closed one. `extern "C"` is not
//     a `noexcept` guarantee, and upstream does not document the C API as
//     exception-free in so many words. What it is, is the narrowest surface
//     available and the one upstream's own non-C++ bindings (python, java,
//     .NET) depend on, so a throw across it would be a bug affecting every
//     Tesseract binding rather than just ours. Recorded in
//     docs/spikes/SP-10-findings.md §6 as the one item that would be closed
//     properly by building libtesseract ourselves with -fno-exceptions, which
//     is part of the same owed hermetic build as everything else in
//     third_party/tesseract/README.md.
//
//  2. **A smaller, more stable surface.** capi.h declares ~120 functions over
//     opaque handles. baseapi.h exposes std::string and std::vector across
//     the boundary, which makes this module's ABI compatibility with
//     libtesseract depend on both being built with the same standard library
//     and the same _LIBCPP_ABI_* settings. For a dependency that
//     third_party/tesseract/README.md expects to be replaced by a
//     cross-compiled build, that is a trap.
//
//  3. **It is the surface a future Android/iOS build keeps.** architecture.md
//     section 4 puts three OCR adapters behind one OCRProviderPort; the C API
//     is the one Tesseract surface that is identical across the NDK, Xcode
//     and this dev host.
//
// Note a wart worth knowing before reading the calls below: capi.h is NOT a
// pure C header when compiled as C++. It includes the C++ publictypes.h, so
// `OEM_LSTM_ONLY`, `PSM_AUTO` and `RIL_PARA` land in namespace `tesseract`
// and must be qualified. That is upstream's arrangement, not ours.
//
// WHAT THIS FILE DELIBERATELY NEVER CALLS
// ---------------------------------------------------------------------------
//   TessBaseAPIProcessPages / ProcessPage   — they take a FILENAME. Under
//       ADR-0002 decision 1 the image is decrypted in memory on the user's
//       device; handing Tesseract a path would mean writing document
//       plaintext to disk for it to read back. Same rule file-engine and
//       pdf-engine already follow (buffer first, path never).
//   TessBaseAPISetImage2 and anything taking a `Pix*` — using it would make
//       this module link Leptonica DIRECTLY and put a Leptonica type in its
//       call graph. SetImage() takes raw bytes, so Leptonica stays a
//       transitive dependency of libtesseract that we never name. That is why
//       ocr/engine/CMakeLists.txt links no `lept`.
//   Any TessResultRenderer (the hOCR / PDF / TSV writers) — spec section 13's
//       searchable PDF is NOT IMPLEMENTED, and Tesseract's own PDF renderer
//       writes a FILE from a FILENAME. See uapp/ocr/engine.h.
//   TessBaseAPIAnalyseLayout without recognition — no consumer wants boxes
//       with no text.

#include "uapp/ocr/engine.h"

#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <tesseract/capi.h>

namespace uapp::ocr {
namespace {

/// Longest tessdata path we will accept, in bytes. Not PATH_MAX (which is
/// 1024 on macOS, 4096 on Linux and not defined at all on some targets): a
/// fixed, generous, platform-independent cap means the same input is accepted
/// or refused everywhere, which is what a cross-platform test suite needs.
constexpr std::size_t kMaxDataPathBytes = 4096;

/// RAII over the `char*` Tesseract hands back from the text accessors, which
/// must be released with TessDeleteText and not with delete[] or free().
///
/// Worth a type rather than a careful `TessDeleteText` at each exit: the
/// per-block loop below has four early-continue paths, and ADR-0001's
/// -fno-exceptions does nothing to protect an early `continue` from leaking.
class TessText final {
public:
    explicit TessText(char* p) noexcept : p_(p) {}
    ~TessText() {
        if (p_ != nullptr) { TessDeleteText(p_); }
    }

    TessText(const TessText&)            = delete;
    TessText& operator=(const TessText&) = delete;
    TessText(TessText&&)                 = delete;
    TessText& operator=(TessText&&)      = delete;

    [[nodiscard]] const char* get() const noexcept { return p_; }

private:
    char* p_ = nullptr;
};

/// RAII over the result iterator, for the same reason.
class TessResultIteratorPtr final {
public:
    explicit TessResultIteratorPtr(TessResultIterator* p) noexcept : p_(p) {}
    ~TessResultIteratorPtr() {
        if (p_ != nullptr) { TessResultIteratorDelete(p_); }
    }

    TessResultIteratorPtr(const TessResultIteratorPtr&)            = delete;
    TessResultIteratorPtr& operator=(const TessResultIteratorPtr&) = delete;
    TessResultIteratorPtr(TessResultIteratorPtr&&)                 = delete;
    TessResultIteratorPtr& operator=(TessResultIteratorPtr&&)      = delete;

    [[nodiscard]] TessResultIterator* get() const noexcept { return p_; }

private:
    TessResultIterator* p_ = nullptr;
};

TessPageIteratorLevel level_for(Granularity g) noexcept {
    switch (g) {
        case Granularity::Block:     return tesseract::RIL_BLOCK;
        case Granularity::Paragraph: return tesseract::RIL_PARA;
        case Granularity::Line:      return tesseract::RIL_TEXTLINE;
        case Granularity::Word:      return tesseract::RIL_WORD;
    }
    // A caller that cast a bogus integer into Granularity gets the documented
    // default rather than an out-of-range value handed to the library.
    return tesseract::RIL_PARA;
}

/// Validate the language tag.
///
/// The character class is the security-relevant part, not the length. This
/// string is concatenated into a filesystem path below (`<path>/<lang>.
/// traineddata`) and handed to Tesseract, which does its own path
/// construction with it. Allowing anything outside [a-z_] would make
/// `"../../../etc/passwd"` and `"eng+deu"` both reach that concatenation —
/// the first is a traversal, the second is a multi-language request this
/// build has not vetted (uapp/ocr/engine.h, Options::language). Rejecting the
/// whole class is one rule instead of two special cases.
Status validate_language(std::string_view language) noexcept {
    if (language.empty() || language.size() > kMaxLanguageTagBytes) {
        return Status::OcrLanguageMalformed;
    }
    for (const char c : language) {
        const bool lower = c >= 'a' && c <= 'z';
        if (!lower && c != '_') { return Status::OcrLanguageMalformed; }
    }
    if (language != kLanguageEnglish) { return Status::OcrLanguageNotImplemented; }
    return Status::Ok;
}

Status validate_data_path(std::string_view path, std::string& out) {
    if (path.empty() || path.size() > kMaxDataPathBytes) { return Status::OcrDataPathInvalid; }
    if (path.find('\0') != std::string_view::npos) {
        // An embedded NUL would truncate at the C boundary, so the bytes we
        // validated would not be the bytes Tesseract opened — the
        // confused-deputy shape file-engine/detection/src/detector_path.cpp
        // guards against for the same reason.
        return Status::OcrDataPathInvalid;
    }
    out.assign(path);
    return Status::Ok;
}

bool is_directory(const std::string& path) noexcept {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) { return false; }
    return S_ISDIR(st.st_mode);
}

bool is_regular_file(const std::string& path) noexcept {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) { return false; }
    return S_ISREG(st.st_mode);
}

/// Where `<language>.traineddata` has to be for Tesseract to find it.
///
/// We check for it OURSELVES rather than letting TessBaseAPIInit2 fail,
/// because Init2 reports every failure as the same -1 and the three
/// conditions behind it need three different responses from the UI — see
/// uapp/status.h's 13xx block. A library that collapsed them would leave a
/// mobile binding unable to tell "your install is broken" from "tap to
/// download Hindi".
std::string model_file_path(const std::string& dir, std::string_view language) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/') { p.push_back('/'); }
    p.append(language);
    p.append(".traineddata");
    return p;
}

/// Strip trailing whitespace. Tesseract terminates every text accessor's
/// result with a newline (two, at block level), which is formatting rather
/// than content: a TextBlock whose text ends in "\n" makes every consumer
/// trim it, and the ones that forget produce a searchable-PDF text layer with
/// phantom line breaks.
///
/// INTERIOR whitespace, newlines included, is left alone — at granularity
/// Paragraph those newlines are the paragraph's own line breaks and they are
/// real layout information (uapp/ocr/engine.h, TextBlock::text).
std::string_view trim_trailing(std::string_view s) noexcept {
    while (!s.empty()) {
        const char c = s.back();
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t' && c != '\f' && c != '\v') { break; }
        s.remove_suffix(1);
    }
    return s;
}

bool is_all_whitespace(std::string_view s) noexcept { return trim_trailing(s).empty(); }

}  // namespace

// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------

Engine::~Engine() { close(); }

Engine::Engine(Engine&& other) noexcept
    : handle_(other.handle_),
      granularity_(other.granularity_),
      source_dpi_(other.source_dpi_),
      min_confidence_(other.min_confidence_) {
    other.handle_ = nullptr;
}

Engine& Engine::operator=(Engine&& other) noexcept {
    if (this != &other) {
        close();
        handle_         = other.handle_;
        granularity_    = other.granularity_;
        source_dpi_     = other.source_dpi_;
        min_confidence_ = other.min_confidence_;
        other.handle_   = nullptr;
    }
    return *this;
}

void Engine::close() noexcept {
    if (handle_ == nullptr) { return; }
    TessBaseAPI* api = static_cast<TessBaseAPI*>(handle_);
    // End() then Delete(): End releases the loaded model and the recognition
    // state, Delete frees the object. Delete alone also works in 5.x, but
    // upstream's documented teardown is the pair and the recommended order,
    // and matching it is free.
    TessBaseAPIEnd(api);
    TessBaseAPIDelete(api);
    handle_ = nullptr;
}

Status Engine::open(const Options& options) {
    // Close first, so a failed open() cannot leave a previously-good engine
    // half-replaced and cannot leave a half-initialised one behind.
    close();

    if (options.source_dpi < kMinSourceDpi || options.source_dpi > kMaxSourceDpi) {
        return Status::InvalidArgument;
    }
    if (options.min_confidence < 0.0f || options.min_confidence > 100.0f) {
        return Status::InvalidArgument;
    }

    // Order matters here and is not arbitrary: the language tag is validated
    // BEFORE it is concatenated into a path, so a traversal attempt never
    // reaches model_file_path() at all.
    const Status lang = validate_language(options.language);
    if (!is_ok(lang)) { return lang; }

    std::string data_path;
    const Status path_status = validate_data_path(options.tessdata_path, data_path);
    if (!is_ok(path_status)) { return path_status; }
    if (!is_directory(data_path)) { return Status::OcrDataPathInvalid; }

    const std::string language(options.language);
    if (!is_regular_file(model_file_path(data_path, language))) {
        return Status::OcrLanguageUnavailable;
    }

    TessBaseAPI* api = TessBaseAPICreate();
    if (api == nullptr) { return Status::OcrEngineInitFailed; }

    // Silence Tesseract's own diagnostics before anything can emit one.
    //
    // This is not cosmetic. Tesseract writes warnings and errors to stderr
    // through tprintf() — "Error opening data file", "Invalid resolution 0
    // dpi", "Empty page" — and three separate project rules are violated by
    // letting it:
    //
    //  * spec section 34 / security-model.md §1: those messages are emitted
    //    while processing a document image, which is A4-class content, and
    //    they include the data path. Logging must be ours to control.
    //  * spec section 33: an error that goes to stderr instead of into a
    //    Status is an error a caller cannot handle.
    //  * a test suite whose expected output includes a third party's
    //    warnings is a test suite nobody reads.
    //
    // "/dev/null" is POSIX. On a Windows host this would need "nul"; Windows
    // is not a target platform in docs/MASTER_PROMPT.md section 3, and
    // guessing at the spelling for a platform we do not build would be worse
    // than this comment.
    if (TessBaseAPISetVariable(api, "debug_file", "/dev/null") == 0) {
        // Non-fatal by design. A Tesseract that will not accept this variable
        // is odd but entirely able to recognise text; refusing to OCR because
        // we could not silence a log would be the wrong trade. The
        // consequence is noisier stderr, which is visible, not silent.
    }

    // OEM_LSTM_ONLY, explicitly, rather than OEM_DEFAULT. Three reasons:
    //
    //  1. OEM_DEFAULT means "whatever the loaded traineddata was built for",
    //    so which recogniser runs would depend on which model file is on the
    //    device. Pinning it means the accuracy numbers we publish describe
    //    the engine we ship.
    //  2. The legacy engine (OEM_TESSERACT_ONLY / _LSTM_COMBINED) needs the
    //    legacy model data, which tessdata_fast does not carry —
    //    performance-strategy.md §1.2 requires tessdata_fast on the device
    //    floor, so asking for the legacy engine would make the build fail on
    //    exactly the tier it is tuned for.
    //  3. The legacy engine is deprecated upstream and slower.
    //
    // The cost of this choice is documented in uapp/ocr/engine.h: it is the
    // reason TextStyle is NOT IMPLEMENTED, since font attributes come only
    // from the legacy engine.
    const int rc = TessBaseAPIInit2(api, data_path.c_str(), language.c_str(),
                                    tesseract::OEM_LSTM_ONLY);
    if (rc != 0) {
        // Every condition we can name was ruled out above, so this is a
        // corrupt or wrong-version model file, or Tesseract running out of
        // memory loading it. Init2 collapses all of those into -1 and
        // inventing finer distinctions than the library can make would be
        // fiction — the same argument uapp/status.h makes for the PDF codes.
        TessBaseAPIDelete(api);
        return Status::OcrEngineInitFailed;
    }

    // PSM_AUTO: full page segmentation and layout analysis, no orientation
    // and script detection.
    //
    // PSM_AUTO rather than PSM_SINGLE_BLOCK because layout analysis is what
    // makes Granularity::Block and Granularity::Paragraph mean anything —
    // spec section 13 asks for "page layout detection" and "paragraph
    // detection" by name, and PSM_SINGLE_BLOCK answers both with "the whole
    // image is one paragraph".
    //
    // Deliberately NOT PSM_AUTO_OSD, which additionally detects page
    // orientation and script. OSD needs osd.traineddata, a SEPARATE 10.5 MB
    // model file on this dev host (measured) — more than twice the size of
    // the eng model it would accompany, against a device-floor budget
    // (performance-strategy.md §1.2) of 4-5 MB for the whole fast language
    // set. Rotation correction belongs in the scanner pipeline, which has the
    // sensor metadata and is NOT IMPLEMENTED. So: upright pages only.
    // Recorded as a limitation in docs/file-format-support.md, not papered
    // over here.
    TessBaseAPISetPageSegMode(api, tesseract::PSM_AUTO);

    handle_         = api;
    granularity_    = options.granularity;
    source_dpi_     = options.source_dpi;
    min_confidence_ = options.min_confidence;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Recognition
// ---------------------------------------------------------------------------

Status Engine::recognise(const ImageView& image, int page, Result& out) const {
    // Clear FIRST, before any early return. A caller reusing one Result
    // across pages must never be able to receive the previous page's blocks
    // alongside an error status.
    out.blocks.clear();
    out.mean_confidence = 0.0f;

    if (handle_ == nullptr) { return Status::OcrNotOpen; }
    if (page < 0) { return Status::InvalidArgument; }

    const Status geometry = image.validate();
    if (!is_ok(geometry)) { return geometry; }

    TessBaseAPI* api = static_cast<TessBaseAPI*>(handle_);

    // The casts are narrowing-checked above: validate() has already proved
    // width and height are in [1, 20000] and that the stride fits the buffer,
    // so neither int cast can lose information. They are written out because
    // -Wconversion is an error in this project and an implicit size_t -> int
    // on a value that has NOT been range-checked is exactly the bug this
    // module is trying not to have.
    const std::size_t stride = image.effective_stride();
    const std::size_t bpp    = image.bytes_per_pixel();

    TessBaseAPISetImage(api, image.pixels, image.width, image.height, static_cast<int>(bpp),
                        static_cast<int>(stride));

    // AFTER SetImage, not before. Tesseract answers a SetSourceResolution
    // call made first with "Please call SetImage before SetSourceResolution."
    // on stderr and keeps its guess (observed on 5.5.3). The ordering is not
    // documented in capi.h, which is why it is written down here.
    TessBaseAPISetSourceResolution(api, source_dpi_);

    if (TessBaseAPIRecognize(api, nullptr) != 0) {
        // nullptr monitor: no progress callback and no cancellation.
        // performance-strategy.md §1.2 requires BOTH for the device floor
        // ("visible progress and cancellation"), and Tesseract supports them
        // through the ETEXT_DESC monitor this argument would carry. NOT
        // IMPLEMENTED: a cancellation token is a cross-module concept
        // (architecture.md section 5 names a cancellable-operation
        // vocabulary) and inventing an OCR-only one would be the wrong shape.
        // Recorded in docs/changelog.md.
        return Status::OcrRecognitionFailed;
    }

    // Read the page-wide mean confidence BEFORE filtering, and before the
    // iterator walk, because it is the one number that answers "was there
    // legible text at all?" independently of what survived min_confidence.
    // See Result::mean_confidence.
    out.mean_confidence = static_cast<float>(TessBaseAPIMeanTextConf(api));

    const TessResultIteratorPtr it(TessBaseAPIGetIterator(api));
    if (it.get() == nullptr) {
        // Tesseract returns a null iterator when there is nothing to iterate.
        // That is a SUCCESSFUL recognition of an image with no text, not a
        // failure — see uapp/ocr/engine.h design rule 6 — so: Ok, no blocks.
        return Status::Ok;
    }

    const TessPageIteratorLevel level = level_for(granularity_);
    std::size_t                  text_bytes = 0;

    do {
        const TessText text(TessResultIteratorGetUTF8Text(it.get(), level));
        if (text.get() == nullptr) {
            // Measured: a blank page yields exactly one iterator position
            // whose text is null and whose confidence is 0.
            continue;
        }

        const std::string_view raw(text.get());
        const std::string_view trimmed = trim_trailing(raw);
        if (trimmed.empty() || is_all_whitespace(trimmed)) { continue; }

        const float confidence = TessResultIteratorConfidence(it.get(), level);
        if (confidence < min_confidence_) { continue; }

        int  left = 0, top = 0, right = 0, bottom = 0;
        const int box_ok = TessPageIteratorBoundingBox(
            TessResultIteratorGetPageIteratorConst(it.get()), level, &left, &top, &right, &bottom);
        if (box_ok == 0) {
            // No coordinates means no TextBlock. Spec section 13's whole
            // point is that "OCR output should retain coordinates"; emitting
            // a block with a zeroed box would be a block whose position is a
            // lie, and the searchable-PDF consumer would position text at the
            // page corner rather than skip it.
            continue;
        }
        if (right <= left || bottom <= top) { continue; }

        if (out.blocks.size() >= kMaxTextBlocks) { return Status::OcrResultTooLarge; }
        if (text_bytes > kMaxResultTextBytes - trimmed.size()) {
            // Written as a subtraction against the cap rather than
            // `text_bytes + trimmed.size() > cap`, so the sum is never
            // computed and cannot wrap. kMaxResultTextBytes is 4 MiB and
            // trimmed.size() is bounded by the recognised page, so the
            // subtraction cannot underflow.
            return Status::OcrResultTooLarge;
        }
        text_bytes += trimmed.size();

        TextBlock block;
        block.text.assign(trimmed);
        block.bounding_box = BoundingBox{left, top, right, bottom};
        block.confidence   = confidence;
        block.page         = page;

        // Read back the language Tesseract says it used, rather than echoing
        // Options::language. See TextBlock::language. The pointer is owned by
        // Tesseract and must NOT be passed to TessDeleteText — note the
        // `const char*` return type, as against the `char*` of the text
        // accessors above. Copied immediately, because its lifetime is tied
        // to the iterator's current position.
        const char* lang = TessResultIteratorWordRecognitionLanguage(it.get());
        if (lang != nullptr) { block.language.assign(lang); }

        // block.style is left default-constructed: TextStyle::available ==
        // false. NOT IMPLEMENTED, deliberately and measurably — see
        // TextStyle in uapp/ocr/engine.h for the evidence.

        out.blocks.push_back(std::move(block));
    } while (TessResultIteratorNext(it.get(), level) != 0);

    return Status::Ok;
}

}  // namespace uapp::ocr
