// SPDX-License-Identifier: Apache-2.0
//
// core/conversion/fidelity.h — the conversion fidelity report.
//
// docs/conversion-architecture.md section 6 makes this the mechanism by which
// the engine keeps its promise not to pretend things work:
//
//     "A reader that swallows an unknown element without emitting an
//      `Unsupported` node is a bug, and a reviewable one -- the parser tests
//      assert that unknown elements produce entries."
//
// This is the first real implementation of that contract. It is the *report*
// half of section 6.1 only; the `Unsupported` IR node of section 6.2 has no
// counterpart yet, because `blocks::BlockContent` is a CLOSED variant with no
// alternative for it (core/blocks/block.h, "WHY A VARIANT"). Where the UDM
// sketch would emit an `Unsupported` node, this converter keeps the construct
// as the plain text it was written in and files a report entry saying so --
// see uapp/conversion/markdown.h, "WHAT IS LOSSY".
//
// WHY ENTRIES AGGREGATE BY (severity, construct)
// ---------------------------------------------------------------------------
// A 40 000-line Markdown file with a table on every line must not produce
// 40 000 report entries: that is an unbounded allocation driven by untrusted
// input, i.e. the thing spec section 38 is about. Aggregating gives a bound of
// one entry per (severity, construct) pair -- a small constant, since the
// construct names are a closed set this module writes -- and the aggregate
// (count + first location) is also what section 6.1's own example report
// shows.
//
// NOT a Status. A fidelity entry is not an error: a conversion that drops a
// table still succeeds and still returns a usable page. Mixing the two would
// force callers to treat "we kept your table as text" as a failure.

#ifndef UAPP_CORE_CONVERSION_FIDELITY_H
#define UAPP_CORE_CONVERSION_FIDELITY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uapp::conversion {

/// The four severities of docs/conversion-architecture.md section 6.1. Values
/// are stable because they will cross the C ABI when one exists.
enum class Fidelity : std::int32_t {
    /// Content was lost. This converter never emits it -- see markdown.h,
    /// "NOTHING IS SILENTLY DROPPED" -- but the severity exists because the
    /// report shape is shared with converters that will.
    Dropped       = 0,
    /// Content survived; its structure or semantics did not.
    Approximated  = 1,
    /// Content and structure survived at reduced quality.
    Degraded      = 2,
    /// Nothing was lost, but the user should know how it was handled.
    PreservedNote = 3,
};

/// Stable lowercase identifier ("dropped", "approximated", ...). Never null;
/// returns "unknown" for an out-of-range integer cast into the enum.
[[nodiscard]] const char* fidelity_name(Fidelity severity) noexcept;

/// One aggregated finding.
struct FidelityEntry final {
    Fidelity    severity = Fidelity::PreservedNote;
    /// Stable snake_case construct id, e.g. "table" or "heading_level_clamped".
    /// A closed vocabulary owned by this module, so tests can assert on it.
    std::string construct;
    /// How many times it occurred. Always >= 1.
    std::size_t count = 0;
    /// 1-based source line of the FIRST occurrence, or 0 when the finding is
    /// not tied to a line (everything on the export side).
    std::size_t first_line = 0;
    /// Non-localised explanation. Contains NO user content, per spec section
    /// 34 -- a report may be logged, and a document body may not be.
    std::string message;
};

/// The accumulated findings of one conversion.
///
/// Deliberately not copy-free or thread-safe: one conversion, one thread, one
/// report, handed back to the caller by reference.
class FidelityReport final {
public:
    /// Record one occurrence. If an entry with the same (severity, construct)
    /// exists, its count is incremented and `first_line`/`message` are left
    /// alone -- the first occurrence is the one worth reporting, and the
    /// message is a property of the construct, not of the occurrence.
    void note(Fidelity severity, std::string_view construct, std::size_t line,
              std::string_view message);

    [[nodiscard]] const std::vector<FidelityEntry>& entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    /// Total occurrences recorded for `construct`, summed over severities.
    /// 0 if it never occurred. This is the accessor the tests use, so that a
    /// test asserts on the construct vocabulary rather than on entry order.
    [[nodiscard]] std::size_t count_of(std::string_view construct) const noexcept;

    /// Total occurrences at `severity`, summed over constructs. The
    /// `summary` object of section 6.1.
    [[nodiscard]] std::size_t count_at(Fidelity severity) const noexcept;

    /// True iff any `Dropped` entry exists. Section 6.3 makes this the
    /// condition for showing the user a notice rather than a quiet note.
    [[nodiscard]] bool lost_content() const noexcept {
        return count_at(Fidelity::Dropped) > 0;
    }

    void clear() noexcept { entries_.clear(); }

private:
    std::vector<FidelityEntry> entries_;
};

}  // namespace uapp::conversion

#endif  // UAPP_CORE_CONVERSION_FIDELITY_H
