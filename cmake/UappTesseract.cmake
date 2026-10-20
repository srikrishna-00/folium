# ---------------------------------------------------------------------------
# cmake/UappTesseract.cmake — define the imported Tesseract target, or explain
# why there isn't one.
#
# This follows cmake/UappSodium.cmake's shape rather than
# cmake/UappPdfium.cmake's, and the choice is argued rather than copied.
# UappPdfium.cmake uses find_library(NO_DEFAULT_PATH) against a vendored,
# SHA-256-pinned tree because a surprise system PDFium would be an unpinned
# parser of unknown vintage on the highest-risk untrusted-input path in the
# product. Tesseract sits between PDFium and libsodium on that axis, and lands
# on the libsodium side for these reasons:
#
#   * Like libsodium, it is a standard package with a pkg-config file that
#     every dev host and CI image can install in one command. There is no
#     upstream prebuilt binary distribution to pin in the first place — no
#     equivalent of bblanchon/pdfium-binaries exists — so NO_DEFAULT_PATH
#     would mean "there is never a Tesseract", not "there is a pinned one".
#   * UNLIKE libsodium, its observable behaviour is NOT checkable against
#     published test vectors: an OCR result is a judgement, not a KAT, so a
#     surprise Tesseract version is a quiet accuracy change rather than a red
#     test. This file therefore reports the version it found and the root
#     CMakeLists prints it, so "which recogniser produced those numbers?" is
#     answerable from a build log. That is weaker than a pin and is stated as
#     such in third_party/tesseract/README.md, "What is still owed".
#
# THIS FILE NEVER TOUCHES THE NETWORK — the one rule it shares unchanged with
# both of the others. There is no FetchContent and no file(DOWNLOAD);
# `cmake -S . -B build` works offline. Acquisition is an explicit prior step
# (`brew install tesseract`, `apt install libtesseract-dev`).
#
# IT ALSO NEVER LOOKS FOR LEPTONICA, which is deliberate and is a property of
# the module rather than an omission. ocr/engine uses TessBaseAPISetImage()
# with raw bytes and never touches a `Pix`, so no Leptonica type appears in
# our call graph and no Leptonica symbol appears in our objects. Leptonica is
# a transitive dependency of libtesseract, resolved by the dynamic linker from
# libtesseract's own load commands. It still needs a THIRD_PARTY_NOTICES
# entry and a licence review — it is linked into the shipped binary either
# way, and docs/spikes/SP-10-findings.md §3 does that review — but it needs no
# line in this file. If a future enhancement pass wants Leptonica's
# preprocessing (deskew, binarisation), that is a NEW dependency edge and gets
# its own imported target and its own argument.
#
# Contract with the rest of the build:
#   UAPP_TESSERACT_FOUND    — TRUE iff uapp::tesseract was defined.
#   uapp::tesseract         — IMPORTED target carrying includes + the library.
#   UAPP_TESSERACT_VERSION  — version string, or "unknown".
#   UAPP_TESSERACT_TESSDATA — a tessdata directory found on this host, or "".
#   UAPP_TESSERACT_REASON   — why it was not found, for the caller's message.
# Nothing here calls message(FATAL_ERROR): whether a missing Tesseract is
# fatal is the root CMakeLists' policy decision, not this file's. Same
# division of responsibility as the other two.
# ---------------------------------------------------------------------------

set(UAPP_TESSERACT_FOUND FALSE)
set(UAPP_TESSERACT_VERSION "unknown")
set(UAPP_TESSERACT_TESSDATA "")
set(UAPP_TESSERACT_REASON "")

# --- route 1: pkg-config ----------------------------------------------------
# Preferred because tesseract.pc is installed by upstream's own CMake install
# and by every distro package, and because it is the only route that reports a
# VERSION. We want the version for a reason stronger than curiosity: the
# tessdata FORMAT is versioned against the engine (a 4.x `traineddata` will
# not load in a build expecting 5.x and vice versa), and OEM_LSTM_ONLY's
# behaviour differs between the two.
#
# QUIET, not REQUIRED: a host with no pkg-config should fall through to route
# 2 rather than fail configure. That is the AUTO contract.
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(UAPP_TESSERACT_PC QUIET tesseract)
endif()

if(UAPP_TESSERACT_PC_FOUND)
    find_library(UAPP_TESSERACT_LIBRARY
        NAMES tesseract
        HINTS ${UAPP_TESSERACT_PC_LIBRARY_DIRS})
    find_path(UAPP_TESSERACT_INCLUDE_DIR
        NAMES tesseract/capi.h
        HINTS ${UAPP_TESSERACT_PC_INCLUDE_DIRS})
    set(UAPP_TESSERACT_VERSION "${UAPP_TESSERACT_PC_VERSION}")
else()
    # --- route 2: plain find_library / find_path ---------------------------
    # For hosts without pkg-config — which includes this dev host: macOS
    # ships none, and Homebrew does not install one as a dependency of
    # tesseract — and for a future cross-compile toolchain file that points
    # CMAKE_PREFIX_PATH at a Tesseract we built ourselves for the NDK or for
    # iOS.
    #
    # The HINTS are Homebrew's two prefixes, because CMake does not search
    # them by default on an Apple Silicon host and the alternative is every
    # developer passing -DCMAKE_PREFIX_PATH by hand. They are HINTS and not
    # PATHS, so a Tesseract found the normal way still wins and a
    # cross-compile toolchain is unaffected.
    find_library(UAPP_TESSERACT_LIBRARY
        NAMES tesseract
        HINTS /opt/homebrew/lib /usr/local/lib)
    find_path(UAPP_TESSERACT_INCLUDE_DIR
        NAMES tesseract/capi.h
        HINTS /opt/homebrew/include /usr/local/include)
endif()

if(NOT UAPP_TESSERACT_LIBRARY OR NOT UAPP_TESSERACT_INCLUDE_DIR)
    set(UAPP_TESSERACT_REASON
        "libtesseract was not found (install it: `brew install tesseract` on macOS, "
        "`apt install libtesseract-dev libleptonica-dev` on Debian/Ubuntu, or point "
        "CMAKE_PREFIX_PATH at a built one). See third_party/tesseract/README.md.")
    # The list above is a CMake list, so it joins with ';' unless collapsed.
    # Same trick UappPdfium.cmake and UappSodium.cmake use, and note that no
    # ';' may appear in the message text itself or it would be eaten here.
    string(REPLACE ";" "" UAPP_TESSERACT_REASON "${UAPP_TESSERACT_REASON}")
    return()
endif()

# --- the version, read out of the installed header -------------------------
# Route 1 supplies one; route 2 does not, and "unknown" is a bad answer to the
# tessdata-format question above. Read it from the header we are about to
# compile against rather than from anything we wrote down, so a hand-modified
# install shows up in the configure log.
if(UAPP_TESSERACT_VERSION STREQUAL "unknown"
   AND EXISTS "${UAPP_TESSERACT_INCLUDE_DIR}/tesseract/version.h")
    file(READ "${UAPP_TESSERACT_INCLUDE_DIR}/tesseract/version.h" uapp_tess_version_header)
    string(REGEX MATCH "TESSERACT_VERSION_STR[ \t]+\"([0-9][^\"]*)\"" _m
           "${uapp_tess_version_header}")
    if(CMAKE_MATCH_1)
        set(UAPP_TESSERACT_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

# --- a tessdata directory, FOR TESTS ONLY ----------------------------------
# ocr/engine deliberately has NO default model path — a compiled-in absolute
# path is wrong in product code and Tesseract's own TESSDATA_PREFIX
# environment fallback would make which model the app uses depend on the
# ambient environment (see Options::tessdata_path). But the TEST suite needs a
# real `eng.traineddata` to recognise anything at all, and hard-coding
# /opt/homebrew/share/tessdata into a test file would make the suite work on
# exactly one machine.
#
# So: locate one here, and hand it to the test target as a compile definition
# (tests/CMakeLists.txt). The library target never sees it. If no tessdata is
# found the module still builds and still links — only the recognition tests
# are skipped, and tests/CMakeLists.txt says so out loud rather than silently
# running a shorter suite.
#
# The pkg-config `datadir` is checked first because that is where the
# Tesseract that we are linking expects its own data to be.
set(uapp_tessdata_candidates "")
if(UAPP_TESSERACT_PC_FOUND)
    pkg_get_variable(uapp_tess_datadir tesseract datadir)
    if(uapp_tess_datadir)
        list(APPEND uapp_tessdata_candidates "${uapp_tess_datadir}/tessdata")
    endif()
endif()
list(APPEND uapp_tessdata_candidates
     /opt/homebrew/share/tessdata
     /usr/local/share/tessdata
     /usr/share/tesseract-ocr/5/tessdata
     /usr/share/tessdata)

foreach(uapp_tess_dir IN LISTS uapp_tessdata_candidates)
    if(EXISTS "${uapp_tess_dir}/eng.traineddata")
        set(UAPP_TESSERACT_TESSDATA "${uapp_tess_dir}")
        break()
    endif()
endforeach()

add_library(uapp_tesseract UNKNOWN IMPORTED GLOBAL)
add_library(uapp::tesseract ALIAS uapp_tesseract)

# UNKNOWN rather than SHARED or STATIC, for the reason UappSodium.cmake gives:
# find_library may hand back either a .dylib/.so or a .a depending on what the
# host installed, and claiming one when it is the other makes CMake generate a
# wrong link line. UNKNOWN means "link this file, I am not asserting its
# kind". The shipping mobile build WILL be static — Apple's App Store rules
# make a bundled .dylib painful and docs/spikes/SP-10-findings.md §4 works
# that through — and can declare STATIC then, because it will own the
# artefact. Note that on THIS host a libtesseract.a does exist beside the
# dylib; find_library prefers the dylib, and that is what the measurements in
# SP-10 describe.
#
# INTERFACE_INCLUDE_DIRECTORIES on an IMPORTED target is treated as SYSTEM by
# CMake, which is load-bearing here for the same reason it is for PDFium and
# libsodium, and MORE so: <tesseract/capi.h> is not a pure C header when
# compiled as C++ — it pulls in publictypes.h, baseapi.h and the C++ standard
# library — and none of that is written to survive our -Wconversion
# -Wold-style-cast -Wdouble-promotion -Werror posture. Our posture is not
# something to relax for a third party's headers.
set_target_properties(uapp_tesseract PROPERTIES
    IMPORTED_LOCATION             "${UAPP_TESSERACT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${UAPP_TESSERACT_INCLUDE_DIR}")

# No CMAKE_BUILD_RPATH manipulation, unlike UappPdfium.cmake. That file has to
# stage a copy of libpdfium and rewrite its install name because the vendored
# dylib's id is the RELATIVE path "./libpdfium.dylib", so the loader would
# resolve it against the process's working directory. A package-manager
# libtesseract has an ABSOLUTE install name (verify:
# `otool -D /opt/homebrew/opt/tesseract/lib/libtesseract.5.dylib`), which the
# loader resolves the same way from anywhere. Nothing to fix, so nothing is
# done — and the difference is recorded so the next reader does not assume
# this file forgot.

set(UAPP_TESSERACT_FOUND TRUE)
