# ---------------------------------------------------------------------------
# cmake/UappStb.cmake — define uapp::stb, a header-only INTERFACE target over
# the vendored stb_image.h / stb_image_write.h.
#
# Same vendoring shape and rationale as cmake/UappHttp.cmake /
# cmake/UappJson.cmake — see third_party/httplib/README.md. Never touches the
# network: the headers are already committed.
#
# stb_image.h / stb_image_write.h are "header-only" in the specific stb sense:
# including the header alone gives you declarations, and exactly ONE
# translation unit in the whole program must additionally
# #define STB_IMAGE_IMPLEMENTATION / STB_IMAGE_WRITE_IMPLEMENTATION before
# including it, to emit the actual function bodies once. That TU is
# pdf-engine/writer/src/stb_impl.cpp — see that file's own comment.
#
# Contract with the rest of the build:
#   uapp::stb — INTERFACE target, include path only, no linked library.
# ---------------------------------------------------------------------------

add_library(uapp_stb INTERFACE)
add_library(uapp::stb ALIAS uapp_stb)

target_include_directories(uapp_stb SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/third_party/stb/include")
