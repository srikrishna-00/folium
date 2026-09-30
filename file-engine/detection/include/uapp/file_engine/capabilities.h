// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/capabilities.h — the spec section 8 capability set.
//
// ---------------------------------------------------------------------------
// THESE FLAGS ARE CLAIMS, AND A WRONG CLAIM IS A BUG
// ---------------------------------------------------------------------------
// `File::capabilities` is what a UI will use to decide whether to show an
// "Edit" button. Granting EDIT for a format we can only read produces a
// button that fails at the point of use, which is worse than no button. So
// the set is not hand-written per format from memory: it is DERIVED from the
// support matrix in docs/file-format-support.md by the rules below, and the
// derivation is written down here so a future matrix edit has an obvious
// corresponding code change.
//
// ---------------------------------------------------------------------------
// MAPPING RULES (matrix column -> capability flag)
// ---------------------------------------------------------------------------
// Five of the ten spec section 8 capabilities have a matrix column with the
// same meaning:
//
//   VIEW      <- matrix VIEW is ✔ or ◐ *and not* ☁. A ☁ (server-only) cell is
//                NOT a client capability: under adr/0002 decision 1 it now
//                means "we ask you, per document, to decrypt and upload it",
//                which is a consent flow, not a thing the file engine can do.
//   EDIT      <- matrix EDIT is ✔ or ◐. "P2" (post-1.0) is NOT a capability.
//   CONVERT   <- matrix CONVERT is ✔ or ◐, and not ☁ / TBD / P2.
//   OCR       <- matrix OCR is ✔ or ◐.
//   ANNOTATE  <- matrix ANNOTATION is ✔ or ◐.
//
// The other five have no matrix column. Spec section 8 names them without
// defining them, so they are defined HERE, and the definitions are the
// deviation recorded in docs/changelog.md and in file-format-support.md §12:
//
//   EXPORT    := the engine can *write this format out*. This is exactly the
//                matrix's CREATE column, so EXPORT <- CREATE is ✔ or ◐.
//                (The alternative reading — "can be saved elsewhere" — is
//                true of every byte sequence and would carry no information.)
//   COMPRESS  := the engine can losslessly repack/optimise the file in place.
//                Granted only where we ship a writer AND recompression is
//                lossless for that container: PDF (QPDF object streams and
//                linearisation) and PNG/ZIP (re-deflate). NOT granted to
//                JPEG: the matrix says re-encoding is lossy and discloses it.
//   SIGN      := apply or validate a cryptographic document signature.
//                **Granted to NOTHING, including PDF.** file-format-support.md
//                §5.1 is explicit: "Digital signature validation is P2/TBD ...
//                A half-implemented signature check is worse than none." A
//                capability flag that lights up a "Sign" affordance is exactly
//                that half-implementation.
//   ENCRYPT   := produce an encrypted copy in the format's own encryption
//                scheme. PDF only (QPDF standard security handler R2–R6). NOT
//                granted to ZIP: the matrix marks AES-encrypted ZIP read-only.
//   EXTRACT   := enumerate and pull out embedded sub-objects (archive members,
//                OOXML parts, PDF attachments). Granted to the ZIP family and
//                to PDF.
//
// ---------------------------------------------------------------------------
// Unknown gets the EMPTY set. A file we could not identify is not "viewable
// as text by default" — that guess is how a renderer gets handed a payload it
// was never meant to see.

#ifndef UAPP_FILE_ENGINE_CAPABILITIES_H
#define UAPP_FILE_ENGINE_CAPABILITIES_H

#include <cstdint>

namespace uapp::files {

/// The ten capabilities of spec section 8, as single bits so a set is one
/// 32-bit word — trivially describable to the future C99 ABI as a uint32_t
/// bitmask rather than ten accessor calls.
enum class Capability : std::uint32_t {
    View     = 1u << 0,
    Edit     = 1u << 1,
    Convert  = 1u << 2,
    Export   = 1u << 3,
    Ocr      = 1u << 4,
    Annotate = 1u << 5,
    Compress = 1u << 6,
    Sign     = 1u << 7,
    Encrypt  = 1u << 8,
    Extract  = 1u << 9,
};

inline constexpr std::uint32_t kCapabilityCount = 10;

/// Stable lowercase name, e.g. "annotate". Never null.
[[nodiscard]] const char* capability_name(Capability capability) noexcept;

/// `index` in [0, kCapabilityCount). Returns Capability::View for an
/// out-of-range index; callers should bound the loop, not rely on that.
[[nodiscard]] Capability capability_at(std::uint32_t index) noexcept;

/// An immutable set of capabilities.
///
/// Immutable on purpose: a capability set is a *claim about a format*, and
/// the only legitimate source of that claim is `format_capabilities()`. A
/// mutable set invites a caller to add EDIT locally and ship a broken button.
class CapabilitySet final {
public:
    constexpr CapabilitySet() noexcept = default;

    explicit constexpr CapabilitySet(std::uint32_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool has(Capability capability) const noexcept {
        return (bits_ & static_cast<std::uint32_t>(capability)) != 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

    /// The raw bitmask, for persistence and for the C ABI.
    [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }

    [[nodiscard]] std::uint32_t count() const noexcept;

    friend constexpr bool operator==(CapabilitySet a, CapabilitySet b) noexcept {
        return a.bits_ == b.bits_;
    }
    friend constexpr bool operator!=(CapabilitySet a, CapabilitySet b) noexcept {
        return !(a == b);
    }

private:
    std::uint32_t bits_ = 0;
};

/// Compose a set at compile time, so the tables in capabilities.cpp read like
/// the matrix rows they are transcribed from.
template <class... Caps>
[[nodiscard]] constexpr CapabilitySet make_capabilities(Caps... caps) noexcept {
    return CapabilitySet((0u | ... | static_cast<std::uint32_t>(caps)));
}

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_CAPABILITIES_H
