#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <windows.h>

namespace myclean::platform::testing {

// A fresh directory under %TEMP% for the current test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::temp_directory_path() /
            ("myclean-platform-service-" + std::to_string(::GetCurrentProcessId()) + "-" +
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

}  // namespace myclean::platform::testing
