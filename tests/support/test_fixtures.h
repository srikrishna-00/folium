// SPDX-License-Identifier: Apache-2.0
//
// tests/support/test_fixtures.h — fakes for the NOTE tests specifically.
//
// The port fakes here implement `notes::Clock` / `notes::IdGenerator`. The
// generic equivalents over `uapp::Clock` / `uapp::IdSource`, and the scratch
// directory helper, now live in common_fixtures.h so that a core/blocks test
// does not have to link core/notes to get a temp directory.

#ifndef UAPP_TESTS_SUPPORT_TEST_FIXTURES_H
#define UAPP_TESTS_SUPPORT_TEST_FIXTURES_H

#include <cstdint>
#include <cstdio>
#include <string>

#include "common_fixtures.h"  // FakeSystemClock, CountingIdSource, TempDir
#include "uapp/notes/ports.h"
#include "uapp/status.h"

namespace uapp::test {

/// A clock the test drives by hand. Lets us assert that modified_at actually
/// advanced on save without sleeping, and makes created_at reproducible.
class FakeClock final : public uapp::notes::Clock {
public:
    explicit FakeClock(std::int64_t start_ms) noexcept : now_(start_ms) {}

    [[nodiscard]] std::int64_t now_unix_ms() const noexcept override { return now_; }

    void advance(std::int64_t delta_ms) noexcept { now_ += delta_ms; }
    void set(std::int64_t ms) noexcept { now_ = ms; }

private:
    std::int64_t now_;
};

/// Deterministic, collision-free ids: a counter in the last byte(s).
/// Sequential ids also make failure output readable.
class CountingIdGenerator final : public uapp::notes::IdGenerator {
public:
    [[nodiscard]] Status generate(uapp::notes::NoteId& out) noexcept override {
        if (fail_next_) {
            fail_next_ = false;
            return Status::Internal;
        }
        ++counter_;
        std::array<std::uint8_t, uapp::notes::kNoteIdBytes> raw{};
        for (std::size_t i = 0; i < 8; ++i) {
            raw[15 - i] = static_cast<std::uint8_t>((counter_ >> (i * 8)) & 0xFFu);
        }
        out = uapp::notes::NoteId(raw);
        return Status::Ok;
    }

    /// Simulate an entropy-source failure exactly once.
    void fail_next() noexcept { fail_next_ = true; }

private:
    std::uint64_t counter_ = 0;
    bool          fail_next_ = false;
};

// TempDir moved to common_fixtures.h (included above) -- it is not
// note-specific and the block storage tests need it too.

}  // namespace uapp::test

#endif  // UAPP_TESTS_SUPPORT_TEST_FIXTURES_H
