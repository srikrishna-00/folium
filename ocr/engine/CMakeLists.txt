add_library(uapp_ocr_engine STATIC
    src/engine.cpp
    src/image_view.cpp)
add_library(uapp::ocr_engine ALIAS uapp_ocr_engine)

target_include_directories(uapp_ocr_engine PUBLIC include)

# uapp::tesseract is PRIVATE, for the same reason uapp::pdfium is PRIVATE on
# uapp_pdf_reader. engine.h holds the TessBaseAPI as a `void*` and the only
# Tesseract header this project includes anywhere is engine.cpp's own
# <tesseract/capi.h> (see that file's header comment) — so a consumer of this
# library gets no Tesseract include path and no compile-time dependency on
# which OCR library sits underneath. architecture.md section 4 names THREE
# OCR adapters behind one OCRProviderPort (Tesseract, Apple Vision, ML Kit); a
# PUBLIC link here would leak today's choice into every future caller and
# make adding the second adapter a project-wide edit instead of a new
# subdirectory.
target_link_libraries(uapp_ocr_engine
    PUBLIC  uapp::core_common
    PRIVATE uapp::tesseract
            uapp::compiler_options)

# ---------------------------------------------------------------------------
# What this target deliberately does NOT link, and why.
#
#   uapp::sqlite3 / uapp::storage_sqlite
#       Same guard rail as core/notes, core/blocks, file-engine/detection and
#       pdf-engine/reader (PHASE0_SUMMARY.md H-3). Recognition is pure: pixels
#       in, TextBlocks out. An OCR engine that can write to the database is
#       one that can be made to by a crafted image.
#
#   uapp::pdf_reader
#       architecture.md section 4: engines depend on ports, never on each
#       other. The "render a PDF page, then OCR it" pipeline this module's
#       ImageView convention exists for (see engine.h, design rule 1) is
#       composed by a CALLER that links both, not by this module calling
#       into pdf-engine.
#
#   uapp::file_detection / uapp::core_blocks / uapp::core_database
#       OCR is a peer engine, not a consumer of the document or detection
#       model. The eventual "recognised text -> searchable content" and
#       "OCR output -> entity extraction" integrations (ai-architecture.md
#       section 13.2) belong in a converter or in ai/, not here.
#
#   Leptonica
#       No Leptonica type or symbol appears in this module's call graph —
#       engine.cpp uses TessBaseAPISetImage() with raw bytes and never
#       touches a `Pix`. Leptonica is a transitive dependency of libtesseract
#       itself, resolved by the dynamic linker from libtesseract's own load
#       commands (see cmake/UappTesseract.cmake). It still gets its own
#       THIRD_PARTY_NOTICES entry because it is linked into the shipped
#       binary either way — just not a target in this file.
# ---------------------------------------------------------------------------
