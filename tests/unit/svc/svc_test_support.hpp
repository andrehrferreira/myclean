#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/store/database.hpp"

namespace myclean::svc::testing {

// A fresh directory under %TEMP% for the current test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::temp_directory_path() /
            ("myclean-svc-" + std::to_string(::GetCurrentProcessId()) + "-" +
             test->test_suite_name() + "-" + test->name());
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

 private:
  std::filesystem::path path_;
};

// A pipe name of this test run only; never the real \\.\pipe\myclean.
inline std::wstring unique_pipe_name() {
  static int counter = 0;
  return LR"(\\.\pipe\myclean-svc-test-)" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(++counter);
}

// The JSON-RPC error code of a raw response.
inline int rpc_code(const Result<std::string>& response) {
  EXPECT_TRUE(response.has_value());
  return response ? nlohmann::json::parse(*response).at("error").at("code").get<int>() : 0;
}

// The single integer `sql` returns from the database at `path`, or -1 after a failure.
inline std::int64_t query_int(const std::filesystem::path& path, std::string_view sql) {
  Result<store::Database> db = store::Database::open(path);
  EXPECT_TRUE(db.has_value());
  if (!db) {
    return -1;
  }
  Result<store::Statement> query = db->prepare(sql);
  EXPECT_TRUE(query.has_value());
  if (!query) {
    return -1;
  }
  const Result<bool> row = query->step();
  return row && *row ? query->column_int(0) : -1;
}

}  // namespace myclean::svc::testing
