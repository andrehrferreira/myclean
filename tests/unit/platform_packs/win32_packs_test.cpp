#include "platform/packs/win32_packs.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

TEST(Win32Machine, ReadsTheMachine) {
  const Win32Machine machine;
  EXPECT_GE(machine.os_build(), 10240U);
  const std::vector<std::string> images = machine.running_images();
  EXPECT_FALSE(images.empty());
  EXPECT_TRUE(std::ranges::any_of(images, [](const std::string& image) {
    return image.ends_with(".exe") || image == "System" || image == "Registry";
  }));
  EXPECT_FALSE(machine.installed_apps().empty());  // Windows registers some programs itself
}

TEST(AdminWriteOnly, AcceptsSystemAndAdministratorsOnly) {
  EXPECT_TRUE(check_admin_write_only(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)"));
  // CREATOR OWNER and inherit-only entries do not apply to the folder itself.
  EXPECT_TRUE(check_admin_write_only(L"O:SYD:(A;OICI;FA;;;SY)(A;OICIIO;GA;;;CO)(A;CIIO;FW;;;BU)"));
}

TEST(AdminWriteOnly, RefusesOtherWritersOwnersAndNoAcl) {
  const Result<void> users = check_admin_write_only(L"O:BAD:(A;OICI;FA;;;BA)(A;CI;0x6;;;BU)");
  ASSERT_FALSE(users);
  EXPECT_EQ(users.error().code, Errc::refused);
  EXPECT_NE(users.error().message.find("S-1-5-32-545 may write"), std::string::npos)
      << users.error().message;
  const Result<void> owner = check_admin_write_only(L"O:BUD:(A;;FA;;;BA)");
  ASSERT_FALSE(owner);
  EXPECT_NE(owner.error().message.find("owned by S-1-5-32-545"), std::string::npos);
  EXPECT_FALSE(check_admin_write_only(L"O:BAD:NO_ACCESS_CONTROL"));
  EXPECT_FALSE(check_admin_write_only(L"O:BAD:(A;;WD;;;WD)"));
}

TEST(OwnerPacksFolder, MissingIsNoneAndAUserFolderIsRefused) {
  const fs::path root = fs::temp_directory_path() / "myclean-it" / "platform-packs";
  fs::remove_all(root);
  const Result<std::optional<fs::path>> missing = owner_packs_folder(root / "packs");
  ASSERT_TRUE(missing) << missing.error().message;
  EXPECT_FALSE(missing->has_value());

  // A folder of %TEMP% belongs to the user running the test: never admin-only.
  fs::create_directories(root / "packs" / "zig");
  const Result<std::optional<fs::path>> user = owner_packs_folder(root / "packs");
  ASSERT_FALSE(user);
  EXPECT_EQ(user.error().code, Errc::refused);
  EXPECT_TRUE(user.error().message.starts_with("owner packs ignored: ")) << user.error().message;
  std::error_code ignored;
  fs::remove_all(root, ignored);
}

TEST(OwnerPacksFolder, IsBelowProgramData) {
  const Result<fs::path> dir = owner_packs_dir();
  ASSERT_TRUE(dir) << dir.error().message;
  EXPECT_EQ(dir->filename(), "packs");
  EXPECT_EQ(dir->parent_path().filename(), "MyClean");
}

}  // namespace
}  // namespace myclean::platform