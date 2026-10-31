#include "platform/fs/protected_locations.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/safety/deny_list.hpp"

namespace myclean::platform {
namespace {

TEST(ProtectedLocations, DescribeThisMachineForTheCallingUser) {
  const safety::ProtectedLocations locations = protected_locations(nullptr).value();
  std::wstring windows(MAX_PATH, L'\0');
  windows.resize(::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())));
  EXPECT_EQ(locations.windows, std::filesystem::path{windows});
  EXPECT_EQ(locations.owner_profile.parent_path(), locations.profiles);
  EXPECT_EQ(locations.owner_known_folders.size(), 5U);
  for (const std::filesystem::path& folder : locations.owner_known_folders) {
    EXPECT_TRUE(folder.is_absolute());
  }
  ASSERT_EQ(locations.myclean.size(), 2U);
  EXPECT_EQ(locations.myclean[0].filename(), L"MyClean");
}

}  // namespace
}  // namespace myclean::platform
