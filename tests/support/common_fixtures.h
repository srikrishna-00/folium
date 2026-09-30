// SPDX-License-Identifier: Apache-2.0
//
// tests/support/common_fixtures.h — fakes for the generic core/common ports,
// plus the scratch-directory helper shared by every storage test.
//
// Split out of test_fixtures.h once core/blocks arrived: the note fixtures
// depend on uapp/notes/ports.h, and a blocks test that included them would
// drag core/notes into a binary that is supposed to prove core/blocks stands
// on its own.
//
// Every std::filesystem call uses the std::error_code overload. The throwing
// overloads call std::terminate under -fno-exceptions, turning a failed
// temp-directory lookup into an unexplained crash instead of a test failure.

#ifndef UAPP_TESTS_SUPPORT_COMMON_FIXTURES_H
#define UAPP_TESTS_SUPPORT_COMMON_FIXTURES_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "uapp/object_id.h"
#include "uapp/ports.h"
#include "uapp/status.h"

namespace uapp::test {

/// A clock the test drives by hand: lets us assert that modified_at actually
/// advanced without sleeping, and makes created_at reproducible.
class FakeSystemClock final : public uapp::Clock {
public:
    explicit FakeSystemClock(std::int64_t start_ms) noexcept : now_(start_ms) {}

    [[nodiscard]] std::int64_t now_unix_ms() const noexcept override { return now_; }

    void advance(std::int64_t delta_ms) noexcept { now_ += delta_ms; }
    void set(std::int64_t ms) noexcept { now_ = ms; }

private:
    std::int64_t now_;
};

/// Deterministic, collision-free ids: a big-endian counter in the low bytes.
/// Sequential ids also make failure output readable.
class CountingIdSource final : public uapp::IdSource {
public:
    [[nodiscard]] Status generate(uapp::ObjectIdBytes& out) noexcept override {
        if (fail_next_) {
            fail_next_ = false;
            return Status::Internal;
        }
        ++counter_;
        uapp::ObjectIdBytes raw{};
        for (std::size_t i = 0; i < 8; ++i) {
            raw[15 - i] = static_cast<std::uint8_t>((counter_ >> (i * 8)) & 0xFFu);
        }
        out = raw;
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
        path_ = base / ("uapp-test-" + std::to_string(counter) + "-" +
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

#endif  // UAPP_TESTS_SUPPORT_COMMON_FIXTURES_H
