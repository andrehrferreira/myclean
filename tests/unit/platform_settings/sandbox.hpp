#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

namespace myclean::platform::testing {

// %TEMP%\myclean-it\settings-<test>: the only place these tests create files. Removed afterwards.
class Sandbox {
 public:
  Sandbox()
      : path_(std::filesystem::temp_directory_path() / "myclean-it" /
              ("settings-" + std::string(current().name()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;
  Sandbox(Sandbox&&) = delete;
  Sandbox& operator=(Sandbox&&) = delete;
  ~Sandbox() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  static const ::testing::TestInfo& current() {
    return *::testing::UnitTest::GetInstance()->current_test_info();
  }

  std::filesystem::path path_;
};

}  // namespace myclean::platform::testing
