// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/spike_status.h — the spike's own error type.
//
// SPIKE CODE. See element.h for why this directory exists and why it does not
// use uapp::Status. Same shape as core/common/status.h (flat enum, stable code
// string, human-readable message, every fallible call [[nodiscard]]) so that
// promoting the module is mechanical; deliberately a DIFFERENT type so that
// experiment-only codes never reach production logs or the C ABI.

#ifndef UAPP_CORE_SYNC_SPIKE_STATUS_H
#define UAPP_CORE_SYNC_SPIKE_STATUS_H

#include <cstdint>

namespace uapp::sync::spike {

enum class SpikeStatus : std::int32_t {
    Ok = 0,

    /// A local insert/delete named an index outside [0, visible_size].
    IndexOutOfRange = 100,

    /// A remote operation could not be applied because the element it depends
    /// on (its parent, its right origin, or its delete target) has not been
    /// delivered yet AND the causal-delivery buffer is full. See
    /// docs/sync-architecture.md section 6.2: the buffer must be BOUNDED, so a
    /// hostile or buggy peer cannot pin memory forever.
    PendingBufferFull = 101,

    /// The document already holds kMaxElements elements.
    ElementLimitExceeded = 102,

    /// The operation is structurally impossible: an insert whose id is null, a
    /// node claiming to be its own parent, a Left-side child of the root.
    OpMalformed = 103,

    /// An invariant of this implementation was violated. A bug in us, never in
    /// the caller's input — exactly the meaning uapp::Status::Internal has.
    Internal = 104,
};

[[nodiscard]] inline constexpr bool is_ok(SpikeStatus s) noexcept {
    return s == SpikeStatus::Ok;
}

/// Stable machine-readable identifier, e.g. "SPIKE_INDEX_OUT_OF_RANGE".
/// Never null.
[[nodiscard]] const char* spike_status_code(SpikeStatus s) noexcept;

/// Human-readable, non-localised explanation. Never null. Carries no user
/// content.
[[nodiscard]] const char* spike_status_message(SpikeStatus s) noexcept;

}  // namespace uapp::sync::spike

#endif  // UAPP_CORE_SYNC_SPIKE_STATUS_H
