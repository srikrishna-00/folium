// SPDX-License-Identifier: Apache-2.0
//
// core/common/ports.h — the ambient-authority ports shared by every domain.
//
// architecture.md section 4 lists ClockPort and RandomPort so that no domain
// calls `time()` or a global RNG directly: timestamps and identifiers become
// deterministic under test, and the host can later supply a platform-approved
// CSPRNG without touching domain code.
//
// `core/notes` declares its own `notes::Clock` / `notes::IdGenerator` for the
// same reason. That duplication is deliberate and temporary: `notes::
// IdGenerator` yields a `notes::NoteId`, so it cannot serve `core/blocks`
// without either templating it or dragging core/notes into core/blocks. The
// generic pair lives here, core/blocks uses it, and core/notes keeps its own
// until the Note-folds-into-Page migration in ADR-0003 retires it. Nothing
// depends on both.

#ifndef UAPP_CORE_COMMON_PORTS_H
#define UAPP_CORE_COMMON_PORTS_H

#include <cstdint>
#include <memory>

#include "uapp/object_id.h"
#include "uapp/status.h"

namespace uapp {

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

/// Identifier source, in raw bytes so that one implementation serves every
/// tagged `ObjectId<T>`.
class IdSource {
public:
    virtual ~IdSource();
    IdSource(const IdSource&) = delete;
    IdSource& operator=(const IdSource&) = delete;

    /// Produce fresh, non-nil identifier bytes. Returns Status::Internal if
    /// the underlying entropy source failed -- we never silently fall back to
    /// a weak source.
    [[nodiscard]] virtual Status generate(ObjectIdBytes& out) noexcept = 0;

protected:
    IdSource() noexcept;
};

/// Default Clock backed by std::chrono::system_clock.
[[nodiscard]] std::unique_ptr<Clock> make_system_clock();

/// Default IdSource seeded from std::random_device.
///
/// Same documented limitation as `notes::make_random_id_generator()`:
/// std::random_device is not guaranteed cryptographically secure on every
/// target. Acceptable while an id is not a capability (single device, no
/// authorisation derived from it). Phase 13 sync makes ids externally
/// visible and must replace this with the platform CSPRNG behind this port.
///
/// NOT IMPLEMENTED: UUIDv7 time-ordered layout (database-design.md section 2).
/// These are 16 random bytes, so they lack the B-tree locality v7 buys. The
/// *shape* is already right, so switching the generator later is a one-file
/// change with no schema or id-format migration.
[[nodiscard]] std::unique_ptr<IdSource> make_random_id_source();

}  // namespace uapp

#endif  // UAPP_CORE_COMMON_PORTS_H
