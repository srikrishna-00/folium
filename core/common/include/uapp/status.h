// SPDX-License-Identifier: Apache-2.0
//
// core/common/status.h — the one error type.
//
// Spec section 33 ("Error handling"): every subsystem must have error codes, a
// human-readable message and a machine-readable code; errors are never
// silently ignored. ADR-0001 forbids exceptions, so *every* fallible operation
// in the core returns a `uapp::Status` and every such return is [[nodiscard]].
//
// architecture.md section 7 describes the eventual richer `uapp_error_t`
// (code + domain + message + cause chain) that the C ABI will expose. This
// slice implements the code + message half of it; the cause chain and the C
// ABI struct are deferred (see docs/changelog.md, "Known gaps").
//
// Codes are a FLAT, APPEND-ONLY enum. Never renumber, never reuse a value:
// these travel into logs, telemetry and (later) the stable C ABI.

#ifndef UAPP_CORE_COMMON_STATUS_H
#define UAPP_CORE_COMMON_STATUS_H

#include <cstdint>

namespace uapp {

enum class Status : std::int32_t {
    Ok = 0,

    // --- generic (1xx) ---
    InvalidArgument = 100,  ///< caller passed something structurally wrong
    NotFound        = 101,
    AlreadyExists   = 102,
    Internal        = 103,  ///< a bug in us, not in the caller's input

    // --- note domain validation (2xx) ---
    NoteTitleTooLong = 200,
    NoteBodyTooLarge = 201,
    NoteIdMalformed  = 202,

    // --- storage port (3xx) ---
    StoreOpenFailed   = 300,
    StoreSchemaFailed = 301,
    StoreReadFailed   = 302,
    StoreWriteFailed  = 303,
    StoreReadOnly     = 304,

    // --- search (4xx) ---
    SearchQueryEmpty = 400,  ///< query had no indexable terms after sanitising
    SearchFailed     = 401,
};

/// True iff the operation succeeded. Prefer `if (!uapp::is_ok(s))` over
/// comparing against Status::Ok by hand.
[[nodiscard]] constexpr bool is_ok(Status s) noexcept { return s == Status::Ok; }

/// Stable machine-readable identifier, e.g. "NOTE_NOT_FOUND". Never null.
/// This is the string that goes into structured logs and telemetry.
[[nodiscard]] const char* status_code(Status s) noexcept;

/// Human-readable, non-localised explanation. Never null. Contains no user
/// content (spec section 34: "Do not collect sensitive document contents in
/// telemetry") -- callers add context, the status never carries a note body.
[[nodiscard]] const char* status_message(Status s) noexcept;

}  // namespace uapp

#endif  // UAPP_CORE_COMMON_STATUS_H
