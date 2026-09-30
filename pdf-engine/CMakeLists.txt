# pdf-engine/ — spec section 11.
#
# Added only when UAPP_ENABLE_PDF is ON (see the root CMakeLists), because
# every target under here needs a vendored PDFium and the rest of the project
# must keep configuring and building for anyone who has not fetched one.
#
# `reader` is the first and, at present, the only piece — and the split is not
# ceremonial. Everything else section 11 lists divides cleanly into two
# groups: things that only READ a document (search, thumbnails, page
# navigation, PDF-to-image) and things that REWRITE one (merge, split,
# rotate, redaction, signatures, encryption). The second group is QPDF's job,
# not PDFium's (file-format-support.md §7: "PDFium ... parse + render",
# "QPDF ... structure, encryption"), and it will be a sibling directory with a
# different dependency. Keeping the reader in its own library means the day a
# writer arrives, "the viewer cannot modify a file" is a property of the build
# graph rather than of everyone's memory.
add_subdirectory(reader)
