// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/text_probe.h"

#include <cstddef>
#include <cstdint>

namespace uapp::files {
namespace {

constexpr std::uint8_t kUtf8Bom[]    = {0xEF, 0xBB, 0xBF};
constexpr std::uint8_t kUtf16LeBom[] = {0xFF, 0xFE};
constexpr std::uint8_t kUtf16BeBom[] = {0xFE, 0xFF};

/// C0 control bytes that occur in ordinary text files.
bool is_benign_control(std::uint8_t b) noexcept {
    return b == 0x09 ||  // TAB
           b == 0x0A ||  // LF
           b == 0x0B ||  // VT
           b == 0x0C ||  // FF
           b == 0x0D;    // CR
}

}  // namespace

bool probe_has_text_bom(ByteSpan probe) noexcept {
    return probe.starts_with(kUtf8Bom, sizeof(kUtf8Bom)) ||
           probe.starts_with(kUtf16LeBom, sizeof(kUtf16LeBom)) ||
           probe.starts_with(kUtf16BeBom, sizeof(kUtf16BeBom));
}

bool probe_is_utf8_text(ByteSpan probe, bool is_complete) noexcept {
    if (probe.empty()) { return false; }

    // A BOM is an explicit declaration by the producer. UTF-16 text contains
    // NUL bytes for every ASCII character, so it has to be decided here,
    // before the NUL rule below rejects it.
    if (probe_has_text_bom(probe)) { return true; }

    const std::size_t n = probe.size();
    std::size_t       i = 0;
    while (i < n) {
        const std::uint8_t b = probe.at_or(i, 0);

        if (b < 0x80) {
            // ASCII. NUL and the non-benign controls are the binary tell.
            if (b == 0x00) { return false; }
            if (b < 0x20 && !is_benign_control(b)) { return false; }
            if (b == 0x7F) { return false; }  // DEL
            ++i;
            continue;
        }

        // Multi-byte sequence. Determine its length and the valid range of
        // its first continuation byte, which is how overlong encodings,
        // surrogates and out-of-range code points are rejected without
        // decoding to a code point at all (the Unicode 15 table 3-7 form).
        std::size_t  length = 0;
        std::uint8_t lo     = 0x80;
        std::uint8_t hi     = 0xBF;

        if (b >= 0xC2 && b <= 0xDF) {
            length = 2;
        } else if (b == 0xE0) {
            length = 3;
            lo     = 0xA0;  // reject overlong 3-byte forms
        } else if (b >= 0xE1 && b <= 0xEC) {
            length = 3;
        } else if (b == 0xED) {
            length = 3;
            hi     = 0x9F;  // reject UTF-16 surrogates U+D800..U+DFFF
        } else if (b >= 0xEE && b <= 0xEF) {
            length = 3;
        } else if (b == 0xF0) {
            length = 4;
            lo     = 0x90;  // reject overlong 4-byte forms
        } else if (b >= 0xF1 && b <= 0xF3) {
            length = 4;
        } else if (b == 0xF4) {
            length = 4;
            hi     = 0x8F;  // reject beyond U+10FFFF
        } else {
            // 0x80-0xC1 (a stray continuation byte, or an overlong lead) and
            // 0xF5-0xFF (never valid in UTF-8).
            return false;
        }

        if (i + length > n) {
            // The sequence runs past the end of what we looked at. If this
            // is a prefix of a larger file, that is expected; if it is the
            // whole file, the file is truncated mid-character.
            return !is_complete;
        }

        const std::uint8_t c1 = probe.at_or(i + 1, 0);
        if (c1 < lo || c1 > hi) { return false; }
        for (std::size_t k = 2; k < length; ++k) {
            const std::uint8_t ck = probe.at_or(i + k, 0);
            if (ck < 0x80 || ck > 0xBF) { return false; }
        }
        i += length;
    }
    return true;
}

}  // namespace uapp::files
