// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/text_probe.h — "is this text or is it binary?"
//
// The one detection in this module with no magic number behind it, and
// therefore the one that has to say out loud that it is a heuristic. The
// support matrix already does: file-format-support.md §2, TXT row —
// "Encoding detection is heuristic (BOM → UTF-8 validity → charset guess).
// Mis-detection possible on legacy 8-bit files."
//
// WHAT THIS IMPLEMENTS, PRECISELY
// ---------------------------------------------------------------------------
//   1. A UTF-8, UTF-16LE or UTF-16BE byte-order mark decides it immediately.
//   2. Otherwise: a NUL byte anywhere in the probe means binary. This single
//      rule rejects the overwhelming majority of binary formats, and no
//      valid UTF-8 text file contains one.
//   3. Otherwise: the bytes must be well-formed UTF-8 — proper continuation
//      counts, no overlong encodings, no surrogate code points, nothing above
//      U+10FFFF. Plain ASCII passes trivially as a subset.
//   4. Control characters outside the set that appears in real text
//      (TAB, LF, VT, FF, CR) mean binary.
//
// WHAT IT DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
//   * Legacy 8-bit charsets (Windows-1252, ISO-8859-x, Shift-JIS). They are
//     NOT IMPLEMENTED here. Guessing between them is a statistical problem
//     that needs frequency tables and a confidence score — ICU's
//     `CharsetDetector` territory — and ICU is planned (§7.1 ADOPT) but not
//     integrated. A Windows-1252 file with accented characters will fail
//     the UTF-8 check and be reported Unknown. That is the honest answer:
//     "we cannot tell what encoding this is" beats mislabelling it UTF-8 and
//     handing mojibake to the editor.
//   * Deciding *which* text format it is (Markdown, JSON, CSV, source). See
//     format_detection_not_implemented_reason() for those.
//
// TRUNCATION
// ---------------------------------------------------------------------------
// We judge a prefix, so a multi-byte sequence may be cut in half at the end
// of the probe. An incomplete sequence in the final three bytes is therefore
// tolerated when the probe is a prefix, and rejected when the probe is known
// to be the whole file — where a dangling sequence is genuine corruption.

#ifndef UAPP_FILE_ENGINE_TEXT_PROBE_H
#define UAPP_FILE_ENGINE_TEXT_PROBE_H

#include "uapp/file_engine/byte_span.h"

namespace uapp::files {

/// True iff `probe` looks like UTF-8 (or ASCII) text.
///
/// `is_complete` must be true only when `probe` is the entire file; it makes
/// the check stricter at the tail. An empty probe is not text.
[[nodiscard]] bool probe_is_utf8_text(ByteSpan probe, bool is_complete) noexcept;

/// True iff `probe` starts with a UTF-8, UTF-16LE or UTF-16BE BOM.
[[nodiscard]] bool probe_has_text_bom(ByteSpan probe) noexcept;

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_TEXT_PROBE_H
