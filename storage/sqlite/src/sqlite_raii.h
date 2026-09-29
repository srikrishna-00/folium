// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/sqlite_raii.h — ownership wrappers for the SQLite C API.
//
// ADR-0001 chose C++ over C specifically so that resource cleanup is a
// destructor and not a ladder of `goto cleanup`. This header is where that
// decision earns its keep: nothing below ever calls sqlite3_finalize() or
// sqlite3_close() explicitly, so no early return can leak a statement and no
// path can double-free one.
//
// Internal to this target. Does not leave storage/sqlite (PHASE0_SUMMARY H-3).

#ifndef UAPP_STORAGE_SQLITE_SQLITE_RAII_H
#define UAPP_STORAGE_SQLITE_SQLITE_RAII_H

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "uapp/status.h"

namespace uapp::storage::sqlite {

/// Owns a sqlite3* connection. Move-only.
class Connection final {
public:
    Connection() noexcept = default;
    explicit Connection(sqlite3* db) noexcept : db_(db) {}

    ~Connection() { reset(nullptr); }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept : db_(other.db_) { other.db_ = nullptr; }
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            reset(other.db_);
            other.db_ = nullptr;
        }
        return *this;
    }

    void reset(sqlite3* db) noexcept {
        if (db_ != nullptr) {
            // close_v2 (not close) so that a still-live statement -- which
            // should be impossible, but we do not bet the process on it --
            // defers the close instead of returning SQLITE_BUSY and leaking.
            (void)sqlite3_close_v2(db_);
        }
        db_ = db;
    }

    [[nodiscard]] sqlite3* get() const noexcept { return db_; }
    [[nodiscard]] explicit operator bool() const noexcept { return db_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
};

/// Owns a prepared sqlite3_stmt*. Move-only. Finalised on destruction.
class Statement final {
public:
    Statement() noexcept = default;
    ~Statement() { reset(); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement(Statement&& other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            reset();
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (stmt_ != nullptr) {
            (void)sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }

    /// Compile `sql`. Returns Status::StoreReadFailed on a compile error --
    /// which, since all our SQL is a compile-time constant, means a bug in us
    /// or a SQLite build missing a feature, never bad user input.
    [[nodiscard]] Status prepare(sqlite3* db, std::string_view sql) noexcept {
        reset();
        if (db == nullptr) { return Status::Internal; }
        if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Status::Internal;
        }
        const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
                                          &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            reset();
            return Status::StoreReadFailed;
        }
        return Status::Ok;
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

    // --- binding -----------------------------------------------------------
    // Every bind checks that the caller's size fits in the `int` the C API
    // takes. Without this, a >2 GiB value would wrap to a negative length and
    // SQLite would read whatever followed the buffer (spec section 38:
    // "Validate buffer sizes", "Check integer overflow").

    [[nodiscard]] Status bind_text(int index, std::string_view value) noexcept {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Status::InvalidArgument;
        }
        // SQLITE_TRANSIENT: SQLite copies immediately, so `value` does not
        // have to outlive the bind. The alternative (SQLITE_STATIC) is a
        // dangling-pointer bug waiting for the first refactor.
        const int rc = sqlite3_bind_text(stmt_, index, value.data(),
                                         static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT);
        return rc == SQLITE_OK ? Status::Ok : Status::StoreWriteFailed;
    }

    [[nodiscard]] Status bind_blob(int index, const void* data, std::size_t len) noexcept {
        if (data == nullptr || len == 0) { return Status::InvalidArgument; }
        if (len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Status::InvalidArgument;
        }
        const int rc = sqlite3_bind_blob(stmt_, index, data, static_cast<int>(len),
                                         SQLITE_TRANSIENT);
        return rc == SQLITE_OK ? Status::Ok : Status::StoreWriteFailed;
    }

    [[nodiscard]] Status bind_int64(int index, std::int64_t value) noexcept {
        const int rc = sqlite3_bind_int64(stmt_, index, value);
        return rc == SQLITE_OK ? Status::Ok : Status::StoreWriteFailed;
    }

    // --- column readers ----------------------------------------------------
    // sqlite3_column_text() may return NULL both for a SQL NULL and for an
    // allocation failure. Either way, an empty string is the only safe
    // reading -- we never construct a std::string from a null pointer.

    [[nodiscard]] std::string column_text(int index) const {
        const auto* bytes = static_cast<const unsigned char*>(
            static_cast<const void*>(sqlite3_column_text(stmt_, index)));
        const int len = sqlite3_column_bytes(stmt_, index);
        if (bytes == nullptr || len <= 0) { return std::string(); }
        return std::string(static_cast<const char*>(static_cast<const void*>(bytes)),
                           static_cast<std::size_t>(len));
    }

    [[nodiscard]] std::int64_t column_int64(int index) const noexcept {
        return sqlite3_column_int64(stmt_, index);
    }

    [[nodiscard]] double column_double(int index) const noexcept {
        return sqlite3_column_double(stmt_, index);
    }

    /// Copy a BLOB column into `out`, which must have room for exactly
    /// `expected_len` bytes. Returns StoreCorrupt-ish (StoreReadFailed) if the
    /// stored blob is a different size -- the schema has a CHECK for this, but
    /// a hand-edited database file is exactly the "never assume an input is
    /// valid" case from spec section 28.
    [[nodiscard]] Status column_fixed_blob(int index,
                                           void* out,
                                           std::size_t expected_len) const noexcept {
        if (out == nullptr || expected_len == 0) { return Status::InvalidArgument; }
        const void* data = sqlite3_column_blob(stmt_, index);
        const int len = sqlite3_column_bytes(stmt_, index);
        if (data == nullptr || len < 0 ||
            static_cast<std::size_t>(len) != expected_len) {
            return Status::StoreReadFailed;
        }
        auto* dst = static_cast<unsigned char*>(out);
        const auto* src = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < expected_len; ++i) { dst[i] = src[i]; }
        return Status::Ok;
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

/// Scoped transaction. Rolls back in the destructor unless commit() succeeded.
///
/// This is what makes "content and index commit together"
/// (database-design.md section 3.9) hold on *every* path, including the error
/// paths, without each method remembering to roll back.
class Transaction final {
public:
    Transaction() noexcept = default;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    ~Transaction() {
        if (db_ != nullptr && !finished_) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    /// BEGIN IMMEDIATE, not BEGIN: take the write lock up front so that a
    /// concurrent writer fails here rather than at COMMIT, after work is done.
    [[nodiscard]] Status begin(sqlite3* db) noexcept {
        if (db == nullptr || db_ != nullptr) { return Status::Internal; }
        const int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) { return Status::StoreWriteFailed; }
        db_ = db;
        finished_ = false;
        return Status::Ok;
    }

    [[nodiscard]] Status commit() noexcept {
        if (db_ == nullptr || finished_) { return Status::Internal; }
        const int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) {
            // Leave finished_ false so the destructor still rolls back.
            return Status::StoreWriteFailed;
        }
        finished_ = true;
        return Status::Ok;
    }

private:
    sqlite3* db_ = nullptr;
    bool finished_ = false;
};

}  // namespace uapp::storage::sqlite

#endif  // UAPP_STORAGE_SQLITE_SQLITE_RAII_H
