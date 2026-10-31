#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace myclean::health::testing {

// Recorded on the reference machine by the X1 spike (docs/spikes/X1-disk-health.md): the NVMe
// SMART / Health log pages and the ATA SMART READ DATA sector, exactly as the drives returned
// them. They hold no serial numbers.
inline std::vector<std::uint8_t> fixture(const std::string& name) {
  const std::filesystem::path path = std::filesystem::path(MYCLEAN_HEALTH_FIXTURES) / name;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << path;
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// A fresh folder under %TEMP% for the current test, removed afterwards.
class TempFolder {
 public:
  TempFolder()
      : path_(std::filesystem::temp_directory_path() /
              ("myclean-health-" +
               std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  TempFolder(TempFolder&&) = delete;
  TempFolder& operator=(TempFolder&&) = delete;
  ~TempFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace myclean::health::testing
