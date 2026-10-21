# pdf-engine/writer — the PDF WRITING surface. See
# include/uapp/pdf/writer.h for the full design rationale; the one-line
# version is: no PDFium dependency at all, deliberately, which is why this
# target is NOT gated by UAPP_PDF_ENABLED the way uapp_pdf_reader is.
add_library(uapp_pdf_writer STATIC
    src/stb_impl.cpp
    src/pdf_writer.cpp)
add_library(uapp::pdf_writer ALIAS uapp_pdf_writer)

target_include_directories(uapp_pdf_writer PUBLIC include)

# uapp::stb is PRIVATE for the same reason uapp::pdfium is PRIVATE on
# uapp_pdf_reader (see that CMakeLists.txt): include/uapp/pdf/writer.h
# never names an stb type, so a consumer of this library gets no stb
# include path and no compile-time dependency on which image codec sits
# underneath.
target_link_libraries(uapp_pdf_writer
    PUBLIC  uapp::core_common
    PRIVATE uapp::stb
            uapp::compiler_options)
