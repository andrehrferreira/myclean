// remove_own_file against the real file system, inside the test's sandbox: it deletes one regular
// file of MyClean's, and refuses deny-listed places, folders and anything reached through a
// junction.

#include "platform/fs/own_files.hpp"

#include <filesystem>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

class OwnFilesTest : public ::testing::Test {
 protected:
  // Deny-list locations that put %WINDIR% and MyClean's own folder inside the sandbox.
  [[nodiscard]] safety::ProtectedLocations locations() const {
    return safety::ProtectedLocations{.windows = root() / "Windows",
                                      .profiles = root() / "Users",
                                      .owner_profile = root() / "Users" / "owner",
                                      .owner_known_folders = {},
                                      .myclean = {root() / "MyClean"}};
  }
  [[nodiscard]] const fs::path& root() const { return sandbox_.root(); }

 private:
  Sandbox sandbox_;
};

TEST_F(OwnFilesTest, DeletesAFileOfMyCleansOwnFolderAndAcceptsOneAlreadyGone) {
  const fs::path dump = root() / "MyClean" / "dumps" / "myclean-svc-1.dmp";
  write_file(dump, 64);

  ASSERT_TRUE(remove_own_file(dump, locations()).has_value());
  EXPECT_FALSE(entry_exists(dump));
  EXPECT_TRUE(remove_own_file(dump, locations()).has_value());
  EXPECT_TRUE(remove_own_file(root() / "no-such-folder" / "x.dmp", locations()).has_value());
}

TEST_F(OwnFilesTest, RefusesADenyListedPlaceAndAFolder) {
  const fs::path system = root() / "Windows" / "System32" / "kernel.dll";
  write_file(system, 8);
  const fs::path folder = root() / "MyClean" / "dumps";
  fs::create_directories(folder);

  const Result<void> denied = remove_own_file(system, locations());
  ASSERT_FALSE(denied.has_value());
  EXPECT_EQ(denied.error().code, Errc::refused);
  EXPECT_TRUE(denied.error().message.starts_with("deny_list:windir"));
  EXPECT_TRUE(entry_exists(system));

  const Result<void> directory = remove_own_file(folder, locations());
  ASSERT_FALSE(directory.has_value());
  EXPECT_EQ(directory.error().code, Errc::refused);
  EXPECT_TRUE(entry_exists(folder));
}

TEST_F(OwnFilesTest, NeverDeletesThroughAJunction) {
  const fs::path target = root() / "elsewhere" / "data.bin";
  write_file(target, 8);
  const fs::path link = root() / "MyClean" / "dumps";
  fs::create_directories(link.parent_path());
  if (!make_junction(link, target.parent_path())) {
    GTEST_SKIP() << "the file system refused the junction";
  }

  const Result<void> refused = remove_own_file(link / "data.bin", locations());
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, Errc::refused);
  EXPECT_TRUE(entry_exists(target));

  const Result<void> junction = remove_own_file(link, locations());
  ASSERT_FALSE(junction.has_value());
  EXPECT_EQ(junction.error().code, Errc::refused);
  EXPECT_TRUE(entry_exists(link));
}

}  // namespace
}  // namespace myclean::platform
