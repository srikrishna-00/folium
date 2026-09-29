// SPDX-License-Identifier: Apache-2.0
//
// core/notes/ports.h — the ambient-authority ports the note domain needs.
//
// architecture.md section 4 lists ClockPort and RandomPort among the ports
// precisely so the domain never calls `time()` or a global RNG directly. Two
// payoffs, both exercised by the unit tests: timestamps and identifiers become
// deterministic under test, and the host can later supply a platform-approved
// CSPRNG without touching domain code.

#ifndef UAPP_CORE_NOTES_PORTS_H
#define UAPP_CORE_NOTES_PORTS_H

#include <cstdint>
#include <memory>

#include "uapp/notes/note.h"
#include "uapp/status.h"

namespace uapp::notes {

/// Wall-clock source. Implementations must be callable from any thread.
class Clock {
public:
    virtual ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    /// Unix epoch milliseconds, UTC. Monotonicity is NOT guaranteed (wall
    /// clocks step); callers must not derive ordering from this alone.
    [[nodiscard]] virtual std::int64_t now_unix_ms() const noexcept = 0;

protected:
    Clock() noexcept;
};

/// Identifier source.
class IdGenerator {
public:
    virtual ~IdGenerator();
    IdGenerator(const IdGenerator&) = delete;
    IdGenerator& operator=(const IdGenerator&) = delete;

    /// Produce a fresh, non-nil identifier. Returns Status::Internal if the
    /// underlying entropy source failed -- we never fall back to a weak
    /// source silently.
    [[nodiscard]] virtual Status generate(NoteId& out) noexcept = 0;

protected:
    IdGenerator() noexcept;
};

/// Default Clock backed by std::chrono::system_clock.
[[nodiscard]] std::unique_ptr<Clock> make_system_clock();

/// Default IdGenerator seeded from std::random_device.
///
/// NOTE (deliberate, documented limitation): std::random_device is not
/// guaranteed to be cryptographically secure on every libstdc++/libc++ target.
/// That is acceptable here because a note id is not a capability -- it is not
/// used for authorisation and it is never exposed to another party in this
/// single-device slice. When sync (Phase 13) makes ids externally visible,
/// this must be replaced by the platform CSPRNG behind the same port.
[[nodiscard]] std::unique_ptr<IdGenerator> make_random_id_generator();

}  // namespace uapp::notes

#endif  // UAPP_CORE_NOTES_PORTS_H
