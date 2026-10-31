#include "core/store/database.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::store {
namespace {

// How long a statement waits for a lock held by another connection before failing with in_use.
constexpr int busy_timeout_ms = 5000;
// At most this many integrity_check findings are quoted in the error.
constexpr std::size_t max_quoted_problems = 10;

Errc errc_for(int code) {
  switch (code & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return Errc::in_use;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
      return Errc::invalid_data;
    default:
      return Errc::io_error;
  }
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

}  // namespace

std::int64_t to_unix_seconds(WallTime time) {
  return std::chrono::floor<std::chrono::seconds>(time.time_since_epoch()).count();
}

WallTime from_unix_seconds(std::int64_t seconds) {
  return WallTime{std::chrono::seconds{seconds}};
}

// --- Statement ---------------------------------------------------------------------------------

Statement::Statement(sqlite3_stmt* statement, sqlite3* connection)
    : statement_(statement), connection_(connection) {}

Statement::Statement(Statement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr)),
      connection_(other.connection_),
      bind_error_(std::move(other.bind_error_)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    sqlite3_finalize(statement_);
    statement_ = std::exchange(other.statement_, nullptr);
    connection_ = other.connection_;
    bind_error_ = std::move(other.bind_error_);
  }
  return *this;
}

Statement::~Statement() {
  sqlite3_finalize(statement_);
}

Statement& Statement::check_bind(int index, int code) {
  if (code != SQLITE_OK && bind_error_.empty()) {
    bind_error_ = "cannot bind parameter " + std::to_string(index) + ": " + sqlite3_errstr(code);
  }
  return *this;
}

Statement& Statement::bind_int(int index, std::int64_t value) {
  return check_bind(index, sqlite3_bind_int64(statement_, index, value));
}

Statement& Statement::bind(int index, double value) {
  return check_bind(index, sqlite3_bind_double(statement_, index, value));
}

Statement& Statement::bind(int index, std::string_view value) {
  // A null data pointer would bind NULL instead of an empty string.
  const char* data = value.empty() ? "" : value.data();
  return check_bind(index, sqlite3_bind_text64(statement_, index, data, value.size(),
                                               SQLITE_TRANSIENT, SQLITE_UTF8));
}

Statement& Statement::bind(int index, std::span<const std::uint8_t> value) {
  if (value.empty()) {
    return check_bind(index, sqlite3_bind_zeroblob(statement_, index, 0));
  }
  return check_bind(
      index, sqlite3_bind_blob64(statement_, index, value.data(), value.size(), SQLITE_TRANSIENT));
}

Statement& Statement::bind(int index, std::nullptr_t) {
  return check_bind(index, sqlite3_bind_null(statement_, index));
}

Statement& Statement::bind_value(int index, const Value& value) {
  return std::visit(
      [&](const auto& held) -> Statement& {
        if constexpr (std::is_same_v<std::decay_t<decltype(held)>, std::string>) {
          return bind(index, std::string_view(held));
        } else {
          return bind(index, held);
        }
      },
      value);
}

Result<bool> Statement::step() {
  if (!bind_error_.empty()) {
    return fail(Errc::invalid_argument, bind_error_);
  }
  const int code = sqlite3_step(statement_);
  if (code == SQLITE_ROW) {
    return true;
  }
  if (code == SQLITE_DONE) {
    return false;
  }
  return fail(errc_for(code),
              std::string(sqlite3_errmsg(connection_)) + " in: " + sqlite3_sql(statement_));
}

Result<void> Statement::run() {
  Result<bool> row = step();
  while (row && *row) {
    row = step();
  }
  sqlite3_reset(statement_);
  sqlite3_clear_bindings(statement_);
  bind_error_.clear();
  if (!row) {
    return tl::unexpected(row.error());
  }
  return {};
}

bool Statement::is_null(int column) const {
  return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::int64_t Statement::column_int(int column) const {
  return sqlite3_column_int64(statement_, column);
}

double Statement::column_double(int column) const {
  return sqlite3_column_double(statement_, column);
}

std::string Statement::column_text(int column) const {
  const unsigned char* text = sqlite3_column_text(statement_, column);
  const int size = sqlite3_column_bytes(statement_, column);
  if (text == nullptr) {
    return {};
  }
  const std::span<const unsigned char> bytes(text, static_cast<std::size_t>(size));
  return {bytes.begin(), bytes.end()};
}

std::vector<std::uint8_t> Statement::column_blob(int column) const {
  const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, column));
  const int size = sqlite3_column_bytes(statement_, column);
  if (data == nullptr) {
    return {};
  }
  const std::span<const std::uint8_t> bytes(data, static_cast<std::size_t>(size));
  return {bytes.begin(), bytes.end()};
}

// --- Database ----------------------------------------------------------------------------------

Database::Database(sqlite3* connection, std::filesystem::path path)
    : connection_(connection), path_(std::move(path)) {}

Database::Database(Database&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)), path_(std::move(other.path_)) {}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    sqlite3_close_v2(connection_);
    connection_ = std::exchange(other.connection_, nullptr);
    path_ = std::move(other.path_);
  }
  return *this;
}

Database::~Database() {
  sqlite3_close_v2(connection_);
}

Result<Database> Database::open(const std::filesystem::path& path) {
  const std::string name = utf8(path);
  sqlite3* connection = nullptr;
  const int code = sqlite3_open_v2(name.c_str(), &connection,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  Database database(connection, path);  // owns the handle even when the open failed
  if (code != SQLITE_OK) {
    return fail(errc_for(code), "cannot open database " + name + ": " + sqlite3_errstr(code));
  }
  sqlite3_busy_timeout(connection, busy_timeout_ms);

  // First, before any statement that could write to the file.
  if (Result<void> healthy = database.check_integrity(); !healthy) {
    return tl::unexpected(healthy.error());
  }

  Result<Statement> journal = database.prepare("PRAGMA journal_mode=WAL");
  if (!journal) {
    return tl::unexpected(journal.error());
  }
  const Result<bool> row = journal->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  const std::string mode = *row ? journal->column_text(0) : std::string{};
  if (mode != "wal") {
    return fail(Errc::io_error,
                "database " + name + " cannot use WAL mode (journal_mode is '" + mode + "')");
  }
  if (Result<void> pragmas = database.exec("PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
      !pragmas) {
    return tl::unexpected(pragmas.error());
  }
  return database;
}

Result<Database> Database::open_in_memory() {
  sqlite3* connection = nullptr;
  const int code =
      sqlite3_open_v2(":memory:", &connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  Database database(connection, ":memory:");
  if (code != SQLITE_OK) {
    return fail(errc_for(code),
                std::string("cannot open in-memory database: ") + sqlite3_errstr(code));
  }
  if (Result<void> pragmas = database.exec("PRAGMA foreign_keys=ON;"); !pragmas) {
    return tl::unexpected(pragmas.error());
  }
  return database;
}

Result<void> Database::check_integrity() {
  const auto failed = [this](const std::string& detail) {
    return fail(Errc::invalid_data,
                "database " + utf8(path_) + " failed PRAGMA integrity_check: " + detail);
  };
  Result<Statement> check = prepare("PRAGMA integrity_check");
  if (!check) {
    return failed(check.error().message);
  }
  std::vector<std::string> problems;
  for (;;) {
    const Result<bool> row = check->step();
    if (!row) {
      return failed(row.error().message);
    }
    if (!*row) {
      break;
    }
    std::string line = check->column_text(0);
    if (line != "ok") {
      problems.push_back(std::move(line));
    }
  }
  if (problems.empty()) {
    return {};
  }
  std::string detail;
  for (std::size_t index = 0; index < problems.size() && index < max_quoted_problems; ++index) {
    detail += (index == 0 ? "" : "; ") + problems[index];
  }
  return failed(detail);
}

Result<void> Database::exec(std::string_view sql) {
  const std::string text(sql);
  char* message = nullptr;
  const int code = sqlite3_exec(connection_, text.c_str(), nullptr, nullptr, &message);
  if (code == SQLITE_OK) {
    return {};
  }
  const std::string detail = message != nullptr ? message : sqlite3_errstr(code);
  sqlite3_free(message);
  return fail(errc_for(code), detail);
}

Result<Statement> Database::prepare(std::string_view sql) {
  sqlite3_stmt* statement = nullptr;
  const int code = sqlite3_prepare_v2(connection_, sql.data(), static_cast<int>(sql.size()),
                                      &statement, nullptr);
  if (code != SQLITE_OK) {
    return fail(errc_for(code),
                std::string(sqlite3_errmsg(connection_)) + " in: " + std::string(sql));
  }
  return Statement(statement, connection_);
}

Result<void> Database::transaction(const std::function<Result<void>()>& work) {
  if (Result<void> begun = exec("SAVEPOINT myclean"); !begun) {
    return begun;
  }
  const auto roll_back = [this] {
    // SQLite may already have rolled the whole transaction back (I/O error, full disk).
    if (sqlite3_get_autocommit(connection_) == 0) {
      static_cast<void>(exec("ROLLBACK TO myclean; RELEASE myclean"));
    }
  };
  Result<void> result;
  try {
    result = work();
  } catch (...) {
    roll_back();
    throw;
  }
  if (result) {
    result = exec("RELEASE myclean");
    if (result) {
      return result;
    }
  }
  roll_back();
  return result;
}

std::int64_t Database::last_insert_id() const {
  return sqlite3_last_insert_rowid(connection_);
}

std::int64_t Database::changes() const {
  return sqlite3_changes64(connection_);
}

}  // namespace myclean::store
