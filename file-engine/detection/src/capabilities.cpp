// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/capabilities.h"

namespace uapp::files {

const char* capability_name(Capability capability) noexcept {
    switch (capability) {
        case Capability::View:     return "view";
        case Capability::Edit:     return "edit";
        case Capability::Convert:  return "convert";
        case Capability::Export:   return "export";
        case Capability::Ocr:      return "ocr";
        case Capability::Annotate: return "annotate";
        case Capability::Compress: return "compress";
        case Capability::Sign:     return "sign";
        case Capability::Encrypt:  return "encrypt";
        case Capability::Extract:  return "extract";
    }
    return "unknown";
}

Capability capability_at(std::uint32_t index) noexcept {
    switch (index) {
        case 0:  return Capability::View;
        case 1:  return Capability::Edit;
        case 2:  return Capability::Convert;
        case 3:  return Capability::Export;
        case 4:  return Capability::Ocr;
        case 5:  return Capability::Annotate;
        case 6:  return Capability::Compress;
        case 7:  return Capability::Sign;
        case 8:  return Capability::Encrypt;
        case 9:  return Capability::Extract;
        default: return Capability::View;
    }
}

std::uint32_t CapabilitySet::count() const noexcept {
    // No std::popcount before C++20, and no intrinsic: this runs once per
    // detected file, not per byte.
    std::uint32_t n    = 0;
    std::uint32_t bits = bits_;
    while (bits != 0) {
        bits &= bits - 1u;
        ++n;
    }
    return n;
}

}  // namespace uapp::files
