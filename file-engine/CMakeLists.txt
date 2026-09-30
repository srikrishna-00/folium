# file-engine/ — the Universal File Engine (spec section 8/9).
#
# Detection is the first and, at present, the only piece. Everything else the
# engine will eventually hold (conversion, thumbnailing, the blob/metadata
# side) depends on being able to answer "what is this?" first, so it lands
# first and alone.
#
# The subdirectory split is not ceremonial: detection must stay a library
# with no parser in it. architecture.md section 5 separates untrusted parsing
# from trusted model construction, and this module is what *decides whether a
# parser should be invoked at all*. If it ever links one, that ordering is
# gone.
add_subdirectory(detection)
