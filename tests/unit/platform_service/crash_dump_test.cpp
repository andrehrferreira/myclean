#include "platform/service/crash_dump.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "temp_dir.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using testing::TempDir;

TEST(CrashDumpTest, WritesADumpOfThisProcess) {
  const TempDir dir;
  const Result<fs::path> dump = write_crash_dump(dir.path(), nullptr);
  ASSERT_TRUE(dump.has_value()) << dump.error().message;
  EXPECT_EQ(dump->parent_path(), dir.path());
  EXPECT_EQ(dump->extension(), ".dmp");
  EXPECT_TRUE(dump->filename().wstring().starts_with(
      L"myclean_platform_service_tests-" + std::to_wstring(::GetCurrentProcessId()) + L"-"));
  std::ifstream written(*dump, std::ios::binary | std::ios::ate);
  EXPECT_GT(written.tellg(), 0);
}

TEST(CrashDumpTest, PruningKeepsTheNewestDumpsAndOtherFiles) {
  const TempDir dir;
  const fs::file_time_type base = fs::file_time_type::clock::now();
  for (int index = 0; index < 5; ++index) {
    const fs::path dump = dir.path() / ("d" + std::to_string(index) + ".dmp");
    std::ofstream(dump) << index;
    fs::last_write_time(dump, base + std::chrono::minutes(index));
  }
  std::ofstream(dir.path() / "notes.txt") << "keep";

  ASSERT_TRUE(prune_crash_dumps(dir.path(), 2).has_value());
  std::set<std::string> left;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir.path())) {
    left.insert(entry.path().filename().string());
  }
  EXPECT_EQ(left, (std::set<std::string>{"d3.dmp", "d4.dmp", "notes.txt"}));
}

}  // namespace
}  // namespace myclean::platform
