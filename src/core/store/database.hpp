#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace myclean::store {

// A statement parameter held by value, for writes that are queued before they run.
using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string>;

// Stored points in time are whole seconds since the Unix epoch (UTC).
[[nodiscard]] std::int64_t to_unix_seconds(WallTime time);
[[nodiscard]] WallTime from_unix_seconds(std::int64_t seconds);

// A prepared SQL statement. Parameters are numbered from 1 (?1, ?2, ...), result columns from 0.
class Statement {
 public:
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;
  ~Statement();

  // Binding never fails on its own: the first binding error is reported by step() or run().
  template <std::integral T>
  Statement& bind(int index, T value) {
    return bind_int(index, static_cast<std::int64_t>(value));
  }
  Statement& bind(int index, double value);
  Statement& bind(int index, std::string_view value);
  Statement& bind(int index, std::span<const std::uint8_t> value);
  Statement& bind(int index, std::nullptr_t);
  Statement& bind_value(int index, const Value& value);

  // Advances to the next result row: true when a row is available, false when done.
  [[nodiscard]] Result<bool> step();
  // Runs the statement to completion, then resets it and clears its bindings for reuse.
  [[nodiscard]] Result<void> run();

  [[nodiscard]] bool is_null(int column) const;
  [[nodiscard]] std::int64_t column_int(int column) const;
  [[nodiscard]] double column_double(int column) const;
  [[nodiscard]] std::string column_text(int column) const;
  [[nodiscard]] std::vector<std::uint8_t> column_blob(int column) const;

 private:
  friend class Database;
  Statement(sqlite3_stmt* statement, sqlite3* connection);

  Statement& bind_int(int index, std::int64_t value);
  Statement& check_bind(int index, int code);

  sqlite3_stmt* statement_;
  sqlite3* connection_;
  std::string bind_error_;
};

// One SQLite connection. Not thread-safe: Store serializes access to it.
class Database {
 public:
  // Opens or creates the database file. Runs PRAGMA integrity_check before anything else and
  // refuses a database that fails it, without writing to the file; then switches to WAL mode and
  // enables foreign keys.
  [[nodiscard]] static Result<Database> open(const std::filesystem::path& path);
  // A private in-memory database (no integrity check, no WAL), used to verify migrations.
  [[nodiscard]] static Result<Database> open_in_memory();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;
  ~Database();

  // Runs one or more statements without parameters.
  [[nodiscard]] Result<void> exec(std::string_view sql);
  [[nodiscard]] Result<Statement> prepare(std::string_view sql);
  // Runs `work` atomically: committed when it succeeds, rolled back when it fails. Nests: inside
  // another transaction it commits or rolls back only its own part (a savepoint).
  [[nodiscard]] Result<void> transaction(const std::function<Result<void>()>& work);

  [[nodiscard]] std::int64_t last_insert_id() const;
  // Rows inserted, updated or deleted by the most recent statement.
  [[nodiscard]] std::int64_t changes() const;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  Database(sqlite3* connection, std::filesystem::path path);

  [[nodiscard]] Result<void> check_integrity();

  sqlite3* connection_;
  std::filesystem::path path_;
};

}  // namespace myclean::store
