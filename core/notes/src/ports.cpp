// SPDX-License-Identifier: Apache-2.0
#include "uapp/notes/ports.h"

#include <chrono>
#include <new>
#include <random>

namespace uapp::notes {

// Out-of-line virtual destructors: keeps each class's vtable emitted in
// exactly one translation unit instead of every includer.
Clock::Clock() noexcept = default;
Clock::~Clock() = default;
IdGenerator::IdGenerator() noexcept = default;
IdGenerator::~IdGenerator() = default;

namespace {

class SystemClock final : public Clock {
public:
    [[nodiscard]] std::int64_t now_unix_ms() const noexcept override {
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(
                            system_clock::now().time_since_epoch())
                            .count();
        // A negative wall clock (pre-1970, or a badly set device clock) would
        // violate validate_note()'s invariant and poison every timestamp
        // written afterwards. Clamp to the epoch rather than persist it.
        return ms < 0 ? std::int64_t{0} : static_cast<std::int64_t>(ms);
    }
};

class RandomIdGenerator final : public IdGenerator {
public:
    [[nodiscard]] Status generate(NoteId& out) noexcept override {
        std::array<std::uint8_t, kNoteIdBytes> raw{};

        // std::random_device::operator() can fail on some platforms. We cannot
        // catch an exception (-fno-exceptions), so we check the one signal we
        // do get -- entropy() == 0 means the implementation is a deterministic
        // PRNG in disguise -- and refuse rather than mint predictable ids.
        std::random_device rd;
        if (rd.entropy() == 0.0 && rd.min() == rd.max()) {
            return Status::Internal;
        }

        std::uniform_int_distribution<unsigned int> dist(0u, 255u);
        for (std::size_t i = 0; i < kNoteIdBytes; ++i) {
            raw[i] = static_cast<std::uint8_t>(dist(rd) & 0xFFu);
        }

        NoteId candidate(raw);
        if (candidate.is_nil()) {
            // Astronomically unlikely (2^-128), but the nil id is our "unset"
            // sentinel, so it must never escape. Force one bit.
            raw[0] = 1u;
            candidate = NoteId(raw);
        }

        out = candidate;
        return Status::Ok;
    }
};

}  // namespace

std::unique_ptr<Clock> make_system_clock() {
    return std::unique_ptr<Clock>(new (std::nothrow) SystemClock());
}

std::unique_ptr<IdGenerator> make_random_id_generator() {
    return std::unique_ptr<IdGenerator>(new (std::nothrow) RandomIdGenerator());
}

}  // namespace uapp::notes
