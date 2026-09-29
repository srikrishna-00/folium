// SPDX-License-Identifier: Apache-2.0
//
// tests/support/test_fixtures.h — fakes and scratch files for the note tests.
//
// Every std::filesystem call here uses the std::error_code overload. The
// throwing overloads would call std::terminate under -fno-exceptions, turning
// a failed temp-directory lookup into an unexplained crash instead of a test
// failure.

#ifndef UAPP_TESTS_SUPPORT_TEST_FIXTURES_H
#define UAPP_TESTS_SUPPORT_TEST_FIXTURES_H

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

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

/// A unique scratch directory, removed on destruction.
class TempDir final {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) { return; }

        static std::uint64_t counter = 0;
        ++counter;
        path_ = base / ("uapp-notes-test-" + std::to_string(counter) + "-" +
                        std::to_string(static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(this))));
        std::filesystem::create_directories(path_, ec);
        if (ec) { path_.clear(); }
    }

    ~TempDir() {
        if (path_.empty()) { return; }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);  // best effort; ec ignored on purpose
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] bool valid() const { return !path_.empty(); }

    /// Path to a file inside the scratch directory. Not created.
    [[nodiscard]] std::string file(const char* name) const {
        return (path_ / name).string();
    }

private:
    std::filesystem::path path_;
};

}  // namespace uapp::test

#endif  // UAPP_TESTS_SUPPORT_TEST_FIXTURES_H
