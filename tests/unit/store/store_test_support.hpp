#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

namespace myclean::store::testing {

// 2026-10-31 12:00:00 UTC.
inline const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

// Success, or a failure message carrying the error.
template <typename T>
::testing::AssertionResult ok(const Result<T>& result) {
  if (result) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << to_string(result.error().code) << ": " << result.error().message;
}

// A fresh directory under %TEMP% for the current test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::temp_directory_path() /
            ("myclean-store-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path database() const { return path_ / "myclean.db"; }

 private:
  std::filesystem::path path_;
};

inline Result<void> exec(Store& store, std::string_view sql) {
  return store.write([&](Database& db) { return db.exec(sql); });
}

// The single integer `sql` returns, or -1 after reporting a failure.
inline std::int64_t query_int(Store& store, std::string_view sql) {
  std::int64_t value = -1;
  const Result<void> read = store.read([&](Database& db) -> Result<void> {
    Result<Statement> query = db.prepare(sql);
    if (!query) {
      return tl::unexpected(query.error());
    }
    const Result<bool> row = query->step();
    if (!row) {
      return tl::unexpected(row.error());
    }
    value = query->column_int(0);
    return {};
  });
  EXPECT_TRUE(ok(read));
  return value;
}

inline std::int64_t count_rows(Store& store, std::string_view table) {
  return query_int(store, "SELECT count(*) FROM " + std::string(table));
}

}  // namespace myclean::store::testing
