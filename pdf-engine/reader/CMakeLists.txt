add_library(uapp_pdf_reader STATIC
    src/document.cpp
    src/library.cpp
    src/render_target.cpp)
add_library(uapp::pdf_reader ALIAS uapp_pdf_reader)

target_include_directories(uapp_pdf_reader PUBLIC include)

# uapp::pdfium is PRIVATE, and that is the important line in this file.
#
# `include/uapp/pdf/document.h` holds the FPDF_DOCUMENT as a `void*` and
# includes no PDFium header, so a consumer of this library gets no PDFium
# include path, no PDFium symbols and no compile-time dependency on which PDF
# library sits underneath. That matters twice over: SP-1 expects the PDFium
# artefact to be replaced (prebuilt bootstrap -> hermetic from-source build),
# and R-12 keeps a commercial MuPDF licence alive as the fallback engine. A
# PUBLIC link here would leak the current choice into every future caller and
# make either move a project-wide edit.
#
# It does mean the link is PRIVATE while libpdfium is a SHARED library, so
# anything linking uapp::pdf_reader must still be able to FIND libpdfium at
# runtime — which is what the rpath carried on the imported target's
# INTERFACE_LINK_OPTIONS handles (cmake/UappPdfium.cmake).
target_link_libraries(uapp_pdf_reader
    PUBLIC  uapp::core_common
    PRIVATE uapp::pdfium
            uapp::compiler_options)

# ---------------------------------------------------------------------------
# What this target deliberately does NOT link, and why.
#
#   uapp::sqlite3 / uapp::storage_sqlite
#       Same guard rail as core/notes, core/blocks and file-engine/detection
#       (PHASE0_SUMMARY.md H-3). Rendering is pure: bytes in, pixels out into
#       a buffer the caller owns. A renderer that can write to the database
#       is a renderer that can be made to write to the database by a
#       malformed file.
#
#   uapp::file_detection
#       architecture.md section 4: engines depend on ports, never on each
#       other's internals. Detection decides *whether* to invoke a parser;
#       this module *is* one. If the reader called the detector, the ordering
#       that keeps untrusted parsing behind a verdict would be inverted.
#       The composition root above both is what wires them together.
#
#   uapp::core_notes / uapp::core_blocks
#       PDF is a peer engine, not a consumer of the document model. The
#       eventual "PDF page -> Page/Block" import path belongs in a converter,
#       not in the renderer.
#
#   QPDF
#       Not vendored, not linked, not needed: nothing here writes a PDF.
#       file-format-support.md §7 assigns structure and encryption to QPDF,
#       and that dependency arrives with the first feature that rewrites a
#       file — not before.
# ---------------------------------------------------------------------------
