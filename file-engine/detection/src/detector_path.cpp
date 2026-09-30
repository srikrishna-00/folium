// SPDX-License-Identifier: Apache-2.0
//
// The only translation unit in this module that touches the filesystem.
//
// It is separate from detector.cpp on purpose. detector.cpp is pure,
// noexcept, allocation-free and therefore directly drivable by a fuzz target
// (architecture.md section 5 rule 4: every parser gets one). Mixing I/O into
// it would make the fuzzable surface need a filesystem.
//
// <cstdio> rather than <fstream>: ADR-0001 builds with -fno-exceptions, and
// iostreams' error reporting is built around either exceptions or stream
// state that is easy to check incorrectly. `std::FILE*` with explicit return
// -value checks matches the Status-returning posture of the rest of the core,
// and is what storage/sqlite already does at its own C boundary.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "uapp/file_engine/detector.h"

namespace uapp::files {
namespace {

/// Bytes hashed per read in describe_path. Big enough that syscall overhead
/// is irrelevant, small enough to sit inside the Tier D arena budget
/// (file-format-support.md §11.1) without a second thought.
constexpr std::size_t kHashChunkBytes = 64u * 1024u;

struct FileCloser final {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) { std::fclose(f); }
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/// A path from a picker or a sync payload is untrusted input like any other.
/// An embedded NUL would truncate at the C boundary, so the bytes we
/// validate would not be the bytes that get opened — the classic confused
/// -deputy shape.
Status validate_path(std::string_view path, std::string& out) {
    if (path.empty()) { return Status::FilePathInvalid; }
    if (path.find('\0') != std::string_view::npos) { return Status::FilePathInvalid; }
    out.assign(path);
    return Status::Ok;
}

Status open_for_read(const std::string& path, FilePtr& out) {
    FilePtr f(std::fopen(path.c_str(), "rb"));
    if (!f) { return Status::FileOpenFailed; }
    out = std::move(f);
    return Status::Ok;
}

/// Size and timestamps in one stat call. Timestamps are seconds in POSIX, so
/// they are widened to the milliseconds the rest of the core uses; the extra
/// precision is simply not available from this source and is not invented.
Status stat_file(const std::string& path, std::uint64_t& size, std::int64_t& created_ms,
                 std::int64_t& modified_ms) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) { return Status::FileOpenFailed; }
    if (!S_ISREG(st.st_mode)) {
        // A directory, socket or device node. Not a file we can detect, and
        // reading one can block forever.
        return Status::FileOpenFailed;
    }
    size        = static_cast<std::uint64_t>(st.st_size);
    modified_ms = static_cast<std::int64_t>(st.st_mtime) * 1000;
    // POSIX has no creation time; st_ctime is inode-change time. Using it as
    // "created_at" would be wrong, so the modification time is carried for
    // both and the caller can override from a platform source that knows
    // better (iOS and Android both expose a real creation date).
    created_ms = modified_ms;
    return Status::Ok;
}

/// Read up to `want` bytes at `offset`. A short read at end-of-file is fine;
/// a read error is not.
Status read_window(std::FILE* f, std::uint64_t offset, std::size_t want,
                   std::vector<std::uint8_t>& out) {
    out.clear();
    if (want == 0) { return Status::Ok; }
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        return Status::FileReadFailed;
    }
    out.resize(want);
    const std::size_t got = std::fread(out.data(), 1, want, f);
    if (got != want && std::ferror(f) != 0) {
        out.clear();
        return Status::FileReadFailed;
    }
    out.resize(got);
    return Status::Ok;
}

void fill_metadata(const DetectionResult& detection, std::string_view name, File& out) {
    out.metadata.claimed_format    = detection.extension_hint;
    out.metadata.extension_present = !out.extension.empty();
    out.metadata.extension_matches_content =
        out.metadata.extension_present && !detection.extension_conflicts;

    // Detection-derived facts only. Format-specific metadata is NOT
    // IMPLEMENTED — see FileMetadata's comment in file.h.
    out.metadata.entries.push_back(
        MetadataEntry{"detection.method", detection_method_name(detection.method)});
    out.metadata.entries.push_back(
        MetadataEntry{"detection.confidence", detection_confidence_name(detection.confidence)});
    if (detection.container_format != FileFormat::Unknown) {
        out.metadata.entries.push_back(
            MetadataEntry{"container.format", format_name(detection.container_format)});
    }
    if (detection.container_truncated) {
        out.metadata.entries.push_back(MetadataEntry{"container.truncated", "true"});
    }
    if (detection.unrecognised_ooxml) {
        out.metadata.entries.push_back(MetadataEntry{"container.ooxml_unrecognised", "true"});
    }
    if (detection.extension_conflicts) {
        out.metadata.entries.push_back(
            MetadataEntry{"extension.conflict_claimed", format_name(detection.extension_hint)});
    }
    if (detection.magic_offset != 0) {
        out.metadata.entries.push_back(
            MetadataEntry{"magic.offset", std::to_string(detection.magic_offset)});
    }
    if (!format_detection_is_implemented(detection.extension_hint) &&
        detection.extension_hint != FileFormat::Unknown &&
        detection.format == FileFormat::Unknown) {
        // The single most useful thing we can say about an unidentified
        // file: we have no sniffer for what its name claims.
        out.metadata.entries.push_back(MetadataEntry{
            "detection.unimplemented_for", format_name(detection.extension_hint)});
    }
    static_cast<void>(name);
}

void finish_file(const DetectionResult& detection, std::string_view name, File& out) {
    out.name      = std::string(name);
    out.extension = extension_of(name);
    out.format    = detection.format;
    out.mime_type = format_mime_type(detection.format);
    // Capabilities come from the registry and the support matrix, never
    // from anything on this instance. See capabilities.h.
    out.capabilities = format_capabilities(detection.format);
    fill_metadata(detection, name, out);
}

}  // namespace

Status detect_path(std::string_view path, DetectionResult& out) {
    out = DetectionResult();

    std::string resolved;
    if (const Status s = validate_path(path, resolved); !is_ok(s)) { return s; }

    std::uint64_t size        = 0;
    std::int64_t  created_ms  = 0;
    std::int64_t  modified_ms = 0;
    if (const Status s = stat_file(resolved, size, created_ms, modified_ms); !is_ok(s)) {
        return s;
    }
    if (size == 0) { return Status::FileEmpty; }

    FilePtr f;
    if (const Status s = open_for_read(resolved, f); !is_ok(s)) { return s; }

    const std::size_t head_want = size < kHeadSampleBytes
                                      ? static_cast<std::size_t>(size)
                                      : kHeadSampleBytes;
    std::vector<std::uint8_t> head;
    if (const Status s = read_window(f.get(), 0, head_want, head); !is_ok(s)) { return s; }
    if (head.empty()) { return Status::FileReadFailed; }

    FileSample sample;
    sample.head       = ByteSpan(head.data(), head.size());
    sample.total_size = size;

    // Only read a tail when the head did not already reach the end. This is
    // what makes a 10 MB DOCX classifiable from its central directory
    // without reading 10 MB.
    std::vector<std::uint8_t> tail;
    if (size > head.size()) {
        const std::size_t tail_want =
            size < kTailSampleBytes ? static_cast<std::size_t>(size) : kTailSampleBytes;
        const std::uint64_t tail_offset = size - tail_want;
        if (const Status s = read_window(f.get(), tail_offset, tail_want, tail); !is_ok(s)) {
            return s;
        }
        if (!tail.empty()) {
            sample.tail        = ByteSpan(tail.data(), tail.size());
            sample.tail_offset = tail_offset;
        }
    }

    DetectionHints hints;
    hints.file_name = file_name_of_path(path);
    return detect_sample(sample, hints, out);
}

Status describe_buffer(ByteSpan buffer, const DetectionHints& hints, const FileId& id,
                       std::int64_t created_at_ms, std::int64_t modified_at_ms, File& out) {
    out = File();

    DetectionResult detection;
    if (const Status s = detect_buffer(buffer, hints, detection); !is_ok(s)) { return s; }

    out.id             = id;
    out.size           = buffer.size();
    out.checksum       = checksum_of(buffer);
    out.created_at_ms  = created_at_ms;
    out.modified_at_ms = modified_at_ms;
    out.location.kind  = FileLocation::Kind::MemoryBuffer;
    finish_file(detection, hints.file_name, out);
    return Status::Ok;
}

Status describe_path(std::string_view path, const FileId& id, File& out) {
    out = File();

    std::string resolved;
    if (const Status s = validate_path(path, resolved); !is_ok(s)) { return s; }

    DetectionResult detection;
    if (const Status s = detect_path(path, detection); !is_ok(s)) { return s; }

    std::uint64_t size        = 0;
    std::int64_t  created_ms  = 0;
    std::int64_t  modified_ms = 0;
    if (const Status s = stat_file(resolved, size, created_ms, modified_ms); !is_ok(s)) {
        return s;
    }

    // The checksum needs every byte, so this is the one path that streams
    // the whole file — in bounded chunks, never holding more than one.
    FilePtr f;
    if (const Status s = open_for_read(resolved, f); !is_ok(s)) { return s; }

    Fnv1a64                   hasher;
    std::vector<std::uint8_t> chunk(kHashChunkBytes);
    std::uint64_t             hashed = 0;
    for (;;) {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), f.get());
        if (got > 0) {
            hasher.update(ByteSpan(chunk.data(), got));
            hashed += got;
        }
        if (got < chunk.size()) {
            if (std::ferror(f.get()) != 0) { return Status::FileReadFailed; }
            break;  // end of file
        }
    }

    out.id = id;
    // The streamed byte count, not the stat size: if the file was truncated
    // between the stat and the read, the digest describes what we actually
    // hashed, and `size` must agree with it or the pair is a lie.
    out.size           = hashed;
    out.checksum       = hasher.finish();
    out.created_at_ms  = created_ms;
    out.modified_at_ms = modified_ms;
    out.location.kind  = FileLocation::Kind::LocalPath;
    out.location.value = resolved;
    finish_file(detection, file_name_of_path(path), out);
    return Status::Ok;
}

}  // namespace uapp::files
