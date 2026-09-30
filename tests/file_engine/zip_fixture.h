// SPDX-License-Identifier: Apache-2.0
//
// tests/file_engine/zip_fixture.h — builds real ZIP archives in memory.
//
// WHY BUILD THEM RATHER THAN CHECK IN A .docx
// ---------------------------------------------------------------------------
// The task allowed either. A builder wins here for three reasons:
//
//  1. **The malformed cases need precision.** "A ZIP whose end-of-central
//     -directory record points past the end of the file" and "a ZIP truncated
//     exactly 12 bytes into its central directory" are the cases that matter,
//     and they are produced exactly by a builder and only approximately by
//     hexediting a checked-in binary.
//  2. **A reviewer can read it.** A 20 KB binary blob in git is an assertion
//     nobody verifies. `add("word/document.xml", "<w:document/>")` states the
//     thing being tested in one line.
//  3. **No fixture drift.** A real Word-produced .docx contains ~15 parts and
//     a theme; if detection ever came to depend on one of them by accident,
//     the fixture would hide it. These archives contain the minimum that
//     makes the format what it is, so the test fails if detection starts
//     relying on anything else.
//
// The archives are genuinely well-formed: correct local headers, a correct
// central directory, a correct EOCD. They use STORED (method 0) rather than
// deflate, which is legal ZIP and keeps the builder free of a compressor.
// CRC-32 fields are zero, which detection does not read — noted here so that
// nobody later mistakes these for archives an extractor would accept.

#ifndef UAPP_TESTS_FILE_ENGINE_ZIP_FIXTURE_H
#define UAPP_TESTS_FILE_ENGINE_ZIP_FIXTURE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uapp::test {

class ZipBuilder final {
public:
    /// Append a STORED member. Order is preserved, which matters for ODF:
    /// its `mimetype` member must be first.
    void add(std::string_view name, std::string_view content) {
        Entry e;
        e.name    = std::string(name);
        e.content = std::string(content);
        e.offset  = static_cast<std::uint32_t>(data_.size());
        entries_.push_back(e);

        put32(data_, 0x04034b50u);                                  // local header sig
        put16(data_, 20);                                           // version needed
        put16(data_, 0);                                            // flags
        put16(data_, 0);                                            // method: STORED
        put16(data_, 0);                                            // mod time
        put16(data_, 0);                                            // mod date
        put32(data_, 0);                                            // crc-32 (unchecked)
        put32(data_, static_cast<std::uint32_t>(e.content.size())); // compressed size
        put32(data_, static_cast<std::uint32_t>(e.content.size())); // uncompressed size
        put16(data_, static_cast<std::uint16_t>(e.name.size()));    // name length
        put16(data_, 0);                                            // extra length
        data_ += e.name;
        data_ += e.content;
    }

    /// The finished archive: members, central directory, EOCD.
    [[nodiscard]] std::string finish() const {
        std::string out = data_;
        const auto  cd_offset = static_cast<std::uint32_t>(out.size());

        for (const Entry& e : entries_) {
            put32(out, 0x02014b50u);                                  // central entry sig
            put16(out, 20);                                           // version made by
            put16(out, 20);                                           // version needed
            put16(out, 0);                                            // flags
            put16(out, 0);                                            // method
            put16(out, 0);                                            // mod time
            put16(out, 0);                                            // mod date
            put32(out, 0);                                            // crc-32
            put32(out, static_cast<std::uint32_t>(e.content.size())); // compressed
            put32(out, static_cast<std::uint32_t>(e.content.size())); // uncompressed
            put16(out, static_cast<std::uint16_t>(e.name.size()));    // name length
            put16(out, 0);                                            // extra length
            put16(out, 0);                                            // comment length
            put16(out, 0);                                            // disk number
            put16(out, 0);                                            // internal attrs
            put32(out, 0);                                            // external attrs
            put32(out, e.offset);                                     // local header offset
            out += e.name;
        }

        const auto cd_size = static_cast<std::uint32_t>(out.size() - cd_offset);
        const auto count   = static_cast<std::uint16_t>(entries_.size());

        put32(out, 0x06054b50u);  // EOCD sig
        put16(out, 0);            // this disk
        put16(out, 0);            // disk with CD
        put16(out, count);        // entries on this disk
        put16(out, count);        // entries total
        put32(out, cd_size);
        put32(out, cd_offset);
        put16(out, 0);            // comment length
        return out;
    }

    /// The members only, with NO central directory and no EOCD — what a
    /// streamed or partially-downloaded archive looks like. Exercises the
    /// local-header fallback path.
    [[nodiscard]] const std::string& members_only() const { return data_; }

    static void put16(std::string& out, std::uint16_t v) {
        out.push_back(static_cast<char>(v & 0xFFu));
        out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    }

    static void put32(std::string& out, std::uint32_t v) {
        out.push_back(static_cast<char>(v & 0xFFu));
        out.push_back(static_cast<char>((v >> 8) & 0xFFu));
        out.push_back(static_cast<char>((v >> 16) & 0xFFu));
        out.push_back(static_cast<char>((v >> 24) & 0xFFu));
    }

private:
    struct Entry final {
        std::string   name;
        std::string   content;
        std::uint32_t offset = 0;
    };

    std::string        data_;
    std::vector<Entry> entries_;
};

// --- the four packages the disambiguation case turns on --------------------
//
// Each holds exactly what makes it that format and nothing else:
// `[Content_Types].xml` marks it an OOXML package, and the family part path
// is what names the family.

[[nodiscard]] inline std::string make_docx() {
    ZipBuilder z;
    z.add("[Content_Types].xml", "<Types/>");
    z.add("_rels/.rels", "<Relationships/>");
    z.add("word/document.xml", "<w:document><w:body/></w:document>");
    return z.finish();
}

[[nodiscard]] inline std::string make_xlsx() {
    ZipBuilder z;
    z.add("[Content_Types].xml", "<Types/>");
    z.add("_rels/.rels", "<Relationships/>");
    z.add("xl/workbook.xml", "<workbook><sheets/></workbook>");
    return z.finish();
}

[[nodiscard]] inline std::string make_pptx() {
    ZipBuilder z;
    z.add("[Content_Types].xml", "<Types/>");
    z.add("_rels/.rels", "<Relationships/>");
    z.add("ppt/presentation.xml", "<p:presentation/>");
    return z.finish();
}

/// ODF: `mimetype` FIRST and STORED, per the ODF spec — which is exactly the
/// property the detector relies on.
[[nodiscard]] inline std::string make_odt() {
    ZipBuilder z;
    z.add("mimetype", "application/vnd.oasis.opendocument.text");
    z.add("META-INF/manifest.xml", "<manifest/>");
    z.add("content.xml", "<office:document-content/>");
    return z.finish();
}

/// A ZIP that is only a ZIP: no OOXML parts, no ODF mimetype.
[[nodiscard]] inline std::string make_plain_zip() {
    ZipBuilder z;
    z.add("holiday/beach.txt", "sand everywhere");
    z.add("holiday/notes.txt", "remember the sunscreen");
    return z.finish();
}

/// An OOXML package of a family we do not recognise — a .vsdx-shaped thing.
/// Should report as Zip, flagged `unrecognised_ooxml`, never guessed at.
[[nodiscard]] inline std::string make_unknown_ooxml() {
    ZipBuilder z;
    z.add("[Content_Types].xml", "<Types/>");
    z.add("visio/document.xml", "<VisioDocument/>");
    return z.finish();
}

}  // namespace uapp::test

#endif  // UAPP_TESTS_FILE_ENGINE_ZIP_FIXTURE_H
