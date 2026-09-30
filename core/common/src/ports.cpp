// SPDX-License-Identifier: Apache-2.0
#include "uapp/ports.h"

#include <chrono>
#include <new>
#include <random>

namespace uapp {

// Out-of-line virtual destructors: one vtable per program instead of one per
// translation unit that includes the header.
Clock::Clock() noexcept = default;
Clock::~Clock() = default;
IdSource::IdSource() noexcept = default;
IdSource::~IdSource() = default;

namespace {

class SystemClock final : public Clock {
public:
    [[nodiscard]] std::int64_t now_unix_ms() const noexcept override {
        using namespace std::chrono;
        const auto ms =
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        // A negative wall clock (badly set device clock) would violate the
        // `>= 0` invariant every timestamp column CHECKs. Clamp rather than
        // persist it.
        return ms < 0 ? std::int64_t{0} : static_cast<std::int64_t>(ms);
    }
};

class RandomIdSource final : public IdSource {
public:
    [[nodiscard]] Status generate(ObjectIdBytes& out) noexcept override {
        // std::random_device::operator() can fail. We cannot catch an
        // exception (-fno-exceptions), so we check the one signal we do get --
        // a zero-entropy, single-valued device is a deterministic PRNG in
        // disguise -- and refuse rather than mint predictable ids.
        std::random_device rd;
        if (rd.entropy() == 0.0 && rd.min() == rd.max()) {
            return Status::Internal;
        }

        ObjectIdBytes raw{};
        std::uniform_int_distribution<unsigned int> dist(0u, 255u);
        for (std::size_t i = 0; i < kObjectIdBytes; ++i) {
            raw[i] = static_cast<std::uint8_t>(dist(rd) & 0xFFu);
        }

        // The nil id is the "unset"/"top level" sentinel, so it must never
        // escape. 2^-128 unlikely, forced anyway.
        if (detail::object_id_is_nil(raw)) { raw[0] = 1u; }

        out = raw;
        return Status::Ok;
    }
};

}  // namespace

std::unique_ptr<Clock> make_system_clock() {
    return std::unique_ptr<Clock>(new (std::nothrow) SystemClock());
}

std::unique_ptr<IdSource> make_random_id_source() {
    return std::unique_ptr<IdSource>(new (std::nothrow) RandomIdSource());
}

}  // namespace uapp
