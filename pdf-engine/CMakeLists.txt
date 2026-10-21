# pdf-engine/ — spec section 11.
#
# This directory is now added UNCONDITIONALLY from the root CMakeLists —
# see the root's own comment at the add_subdirectory(pdf-engine) call for
# why. What's still conditional is `reader` specifically, since every
# target under IT needs a vendored PDFium and the rest of the project must
# keep configuring and building for anyone who has not fetched one.
#
# `reader` and `writer` split cleanly along the line this directory's own
# comment predicted before `writer` existed: things that only READ a
# document (search, thumbnails, page navigation, PDF-to-image) and things
# that WRITE one (images-to-PDF, and — composed with reader's render_page()
# — a rasterizing merge/split/rotate/organize; see
# pdf-engine/writer/include/uapp/pdf/writer.h for exactly what that module
# does and does not do). `writer` has no PDFium dependency at all (it is its
# own minimal PDF syntax generator over stb_image for JPEG), which is why it
# is unconditional here while `reader` stays gated.
if(UAPP_PDF_ENABLED)
    add_subdirectory(reader)
endif()
add_subdirectory(writer)
