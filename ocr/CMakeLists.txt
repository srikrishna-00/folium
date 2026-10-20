# ocr/ — spec section 13.
#
# Added only when UAPP_ENABLE_OCR is ON (see the root CMakeLists), because
# every target under here needs a system Tesseract and the rest of the
# project must keep configuring and building for anyone who has not
# installed one.
#
# `engine` is the first and, at present, the only piece. Spec section 13's
# other named cases — tables, forms, receipts/business cards, handwriting —
# are each either a layout pass over `engine`'s TextBlock output or a model
# this build does not have, not a sibling recognition engine, so there is no
# second subdirectory to add yet. See ocr/engine/include/uapp/ocr/engine.h
# for the full scoping argument.
add_subdirectory(engine)
