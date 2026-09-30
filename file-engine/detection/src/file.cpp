// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/file.h"

#include <cstddef>

namespace uapp::files {
namespace {

/// Longest suffix treated as an extension. Beyond this it is not an
/// extension, it is a filename containing a dot, and comparing a 4000-byte
/// "extension" against the registry is pure waste.
constexpr std::size_t kMaxExtensionBytes = 16;

}  // namespace

const char* file_location_kind_name(FileLocation::Kind kind) noexcept {
    switch (kind) {
        case FileLocation::Kind::Unknown:      return "unknown";
        case FileLocation::Kind::LocalPath:    return "local_path";
        case FileLocation::Kind::MemoryBuffer: return "memory_buffer";
        case FileLocation::Kind::PlatformUri:  return "platform_uri";
    }
    return "unknown";
}

std::string_view FileMetadata::find(std::string_view key) const noexcept {
    for (const MetadataEntry& e : entries) {
        if (e.key == key) { return e.value; }
    }
    return std::string_view();
}

std::string_view file_name_of_path(std::string_view path) noexcept {
    // Both separators, because a Windows-style path can reach us through a
    // sync payload or a ZIP member name even on a POSIX host.
    const std::size_t slash     = path.find_last_of('/');
    const std::size_t backslash = path.find_last_of('\\');
    std::size_t       cut       = std::string_view::npos;
    if (slash != std::string_view::npos) { cut = slash; }
    if (backslash != std::string_view::npos && (cut == std::string_view::npos || backslash > cut)) {
        cut = backslash;
    }
    if (cut == std::string_view::npos) { return path; }
    return path.substr(cut + 1);
}

std::string extension_of(std::string_view name) {
    const std::string_view base = file_name_of_path(name);
    if (base.empty()) { return std::string(); }

    const std::size_t dot = base.find_last_of('.');
    // No dot at all, or a leading dot with nothing before it: ".bashrc" is a
    // name, not an extension. A trailing dot leaves an empty suffix.
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= base.size()) {
        return std::string();
    }

    const std::string_view suffix = base.substr(dot + 1);
    if (suffix.size() > kMaxExtensionBytes) { return std::string(); }

    std::string out;
    out.reserve(suffix.size());
    for (const char c : suffix) {
        // ASCII lowercase only. A locale-aware tolower on untrusted bytes is
        // how a Turkish-locale build ends up failing to recognise ".TIF".
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

}  // namespace uapp::files
