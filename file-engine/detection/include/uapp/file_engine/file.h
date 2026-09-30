// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/file.h — the spec section 8 File abstraction.
//
// Spec section 8 specifies exactly these eleven fields:
//
//     File { id name extension mime_type size checksum created_at
//            modified_at location metadata capabilities }
//
// Every one of them is here, as a real typed value. Notes on the four that
// needed a decision rather than a transcription:
//
//   location    — spec §8 says "location" without saying of what kind. On
//                 mobile there is frequently no path at all: a picked file
//                 arrives as a security-scoped URI or as bytes. So this is a
//                 tagged `FileLocation`, not a string. A `std::string path`
//                 would have forced every mobile call site to invent a fake
//                 one, and a fake path is a path something will eventually
//                 try to open.
//
//   metadata    — deliberately NOT a generic string map pretending to be
//                 rich. Format-specific metadata (EXIF, XMP, the PDF Info
//                 dictionary, OOXML core properties) requires the parsers
//                 this run does not build. What is here is what detection
//                 legitimately knows; see FileMetadata.
//
//   checksum    — see checksum.h for the algorithm choice and its limits.
//
//   capabilities— derived from docs/file-format-support.md by the rules in
//                 capabilities.h. Never hand-set on an instance.
//
// A `File` is a plain value: constructed by `describe_*` in detector.h,
// copyable, comparable, with no behaviour of its own. It is not a handle to
// an open file and holds no descriptor.

#ifndef UAPP_FILE_ENGINE_FILE_H
#define UAPP_FILE_ENGINE_FILE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/file_engine/capabilities.h"
#include "uapp/file_engine/checksum.h"
#include "uapp/file_engine/format.h"
#include "uapp/object_id.h"

namespace uapp::files {

struct FileIdTag;

/// The same 16-byte identifier every other domain uses, tagged so a FileId
/// cannot be passed where a PageId or BlockId is expected.
using FileId = ObjectId<FileIdTag>;

/// Where the bytes came from. Tagged because "a path" is not universal.
struct FileLocation final {
    enum class Kind : std::int32_t {
        /// No location: the value was built from bytes with no provenance.
        Unknown = 0,
        /// A filesystem path we can reopen. Desktop, and Android app-private
        /// storage.
        LocalPath = 1,
        /// Bytes handed to us with no durable location — a picker result, a
        /// pasteboard item, an upload body. Re-reading is not possible.
        MemoryBuffer = 2,
        /// A platform URI (Android `content://`, iOS security-scoped URL).
        /// Resolvable only through the host, and possibly only once.
        ///
        /// NOT IMPLEMENTED: resolving one. That needs the platform vtable
        /// (architecture.md section 4's downward injection), which does not
        /// exist yet. The kind is declared so hosts can round-trip the value
        /// today without it being mistaken for a path.
        PlatformUri = 3,
    };

    Kind        kind = Kind::Unknown;
    /// The path or URI. Empty for Unknown and MemoryBuffer. Never
    /// interpreted by this module — it is carried, not resolved.
    std::string value;

    friend bool operator==(const FileLocation& a, const FileLocation& b) {
        return a.kind == b.kind && a.value == b.value;
    }
};

[[nodiscard]] const char* file_location_kind_name(FileLocation::Kind kind) noexcept;

/// One metadata key/value. Keys are stable lowercase identifiers.
struct MetadataEntry final {
    std::string key;
    std::string value;

    friend bool operator==(const MetadataEntry& a, const MetadataEntry& b) {
        return a.key == b.key && a.value == b.value;
    }
};

/// Spec section 8's `metadata`, as far as *detection* can honestly fill it.
///
/// NOT IMPLEMENTED: format-specific metadata — EXIF/XMP for images, the PDF
/// Info dictionary and XMP packet, OOXML `docProps/core.xml`, ID3 for audio.
/// Every one of those requires opening and parsing the format, which is out
/// of scope for a detector by design (architecture.md section 5: parsers are
/// a separate, fuzzed, arena-bounded layer). Reading them here would put an
/// untrusted-input parser inside the module whose whole job is to decide
/// whether a parser should be invoked at all.
///
/// `entries` therefore currently carries only detection-derived facts, and
/// is a vector rather than a map because the set is small, ordered output is
/// nicer to log, and it maps to a C array across the future ABI.
struct FileMetadata final {
    /// True iff the filename extension agreed with the detected format.
    /// False when they disagreed OR when the name carried no usable
    /// extension — `extension_present` disambiguates.
    bool extension_matches_content = false;
    bool extension_present         = false;

    /// What the name claimed, whether or not it was right.
    FileFormat claimed_format = FileFormat::Unknown;

    std::vector<MetadataEntry> entries;

    /// Value for `key`, or empty if absent.
    [[nodiscard]] std::string_view find(std::string_view key) const noexcept;
};

/// The spec section 8 File.
struct File final {
    FileId        id;
    /// The display name as given, including its extension. May be empty for
    /// a buffer with no name. NEVER used for detection.
    std::string   name;
    /// Lowercase, without a dot. Derived from `name`; empty if it had none.
    std::string   extension;
    /// The media type of the DETECTED format, not of the claimed one.
    std::string   mime_type;
    std::uint64_t size = 0;
    Checksum      checksum;
    std::int64_t  created_at_ms  = 0;
    std::int64_t  modified_at_ms = 0;
    FileLocation  location;
    FileMetadata  metadata;
    CapabilitySet capabilities;

    /// The detected format. Spec section 8's field list does not name it
    /// separately — `mime_type` is its external face — but carrying the enum
    /// too means callers switch on a value rather than on a string.
    FileFormat format = FileFormat::Unknown;
};

/// Lowercase extension of `name`, without the dot, or empty.
///
/// Handles the cases a naive `rfind('.')` gets wrong: a dotfile (".bashrc"
/// has no extension, it is a name), a trailing dot, a dot that appears only
/// in a directory component ("/a.b/c"), and an absurdly long suffix. It does
/// NOT strip double extensions: for "archive.tar.gz" the extension is "gz",
/// because that is what the last component claims.
[[nodiscard]] std::string extension_of(std::string_view name);

/// The final path component of `path`, handling both separators. Returns the
/// whole input when there is no separator.
[[nodiscard]] std::string_view file_name_of_path(std::string_view path) noexcept;

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_FILE_H
