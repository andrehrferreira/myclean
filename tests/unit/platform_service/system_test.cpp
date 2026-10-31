#include "platform/service/system.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
// clang-format off
#include <windows.h>  // first: sddl.h needs its types
#include <sddl.h>
// clang-format on

#include "core/model/result.hpp"
#include "platform/ipc/security.hpp"
#include "temp_dir.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using testing::TempDir;

constexpr std::string_view owner = "S-1-5-21-1000000000-2000000000-3000000000-1001";
constexpr BYTE inherit = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
constexpr DWORD file_generic_execute = FILE_GENERIC_EXECUTE;
constexpr DWORD read_execute = file_generic_read | file_generic_execute;

bool is_elevated() {
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  return ::GetTokenInformation(::GetCurrentProcessToken(), TokenElevation, &elevation,
                               sizeof(elevation), &size) != FALSE &&
         elevation.TokenIsElevated != 0;
}

std::string sid_text(PSID sid) {
  wil::unique_hlocal_string text;
  if (::ConvertSidToStringSidW(sid, text.put()) == FALSE) {
    return {};
  }
  std::string result;
  for (const wchar_t c : std::wstring_view(text.get())) {
    result.push_back(static_cast<char>(c));  // SIDs are ASCII
  }
  return result;
}

void expect_state_aces(const std::vector<AllowedAce>& aces, BYTE flags) {
  ASSERT_EQ(aces.size(), 3U);
  EXPECT_EQ(aces[0].sid, system_sid);
  EXPECT_EQ(aces[0].mask, static_cast<DWORD>(FILE_ALL_ACCESS));
  EXPECT_EQ(aces[1].sid, administrators_sid);
  EXPECT_EQ(aces[1].mask, static_cast<DWORD>(FILE_ALL_ACCESS));
  EXPECT_EQ(aces[2].sid, owner);
  EXPECT_EQ(aces[2].mask, read_execute);
  for (const AllowedAce& ace : aces) {
    EXPECT_EQ(ace.flags, flags) << ace.sid;
  }
}

TEST(StateDirTest, SecurityIsAdministratorsOwnedAndOwnerReadOnly) {
  Result<wil::unique_hlocal_security_descriptor> descriptor =
      make_security_descriptor(state_dir_sddl(owner));
  ASSERT_TRUE(descriptor.has_value());

  PSID sd_owner = nullptr;
  BOOL defaulted = FALSE;
  ASSERT_TRUE(::GetSecurityDescriptorOwner(descriptor->get(), &sd_owner, &defaulted));
  EXPECT_EQ(sid_text(sd_owner), administrators_sid);

  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  ASSERT_TRUE(::GetSecurityDescriptorControl(descriptor->get(), &control, &revision));
  EXPECT_NE(control & SE_DACL_PROTECTED, 0);

  const Result<std::vector<AllowedAce>> aces = allowed_aces(descriptor->get());
  ASSERT_TRUE(aces.has_value());
  expect_state_aces(*aces, inherit);
}

// Changing the owner to Administrators needs an elevated token, as the service has.
TEST(StateDirTest, ProtectingResetsTheFolderAndEverythingBelow) {
  if (!is_elevated()) {
    GTEST_SKIP() << "needs an elevated token";
  }
  const TempDir dir;
  const fs::path planted = dir.path() / "logs" / "planted.txt";
  fs::create_directories(planted.parent_path());
  std::ofstream(planted) << "x";
  // An explicit grant to Everyone that the reset must remove.
  Result<wil::unique_hlocal_security_descriptor> open = make_security_descriptor("D:(A;;FA;;;WD)");
  ASSERT_TRUE(open.has_value());
  ASSERT_TRUE(::SetFileSecurityW(planted.c_str(), DACL_SECURITY_INFORMATION, open->get()));

  ASSERT_TRUE(protect_state_dir(dir.path(), owner).has_value());

  for (const fs::path& path : {dir.path(), planted}) {
    const wil::unique_hfile handle(::CreateFileW(path.c_str(), READ_CONTROL, FILE_SHARE_READ,
                                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                                 nullptr));
    ASSERT_TRUE(handle.is_valid());
    const Result<std::vector<AllowedAce>> aces = object_aces(handle.get());
    ASSERT_TRUE(aces.has_value()) << aces.error().message;
    if (path == planted) {
      expect_state_aces(*aces, INHERITED_ACE);  // a file: inherited, nothing explicit left
    } else {
      expect_state_aces(*aces, inherit);
    }
    const Result<std::string> file_owner = file_owner_sid(path);
    ASSERT_TRUE(file_owner.has_value());
    EXPECT_EQ(*file_owner, administrators_sid);
  }
}

TEST(SystemTest, FindsKnownFoldersAndThisExecutable) {
  const Result<fs::path> program_data = program_data_dir();
  ASSERT_TRUE(program_data.has_value());
  EXPECT_TRUE(program_data->is_absolute());
  EXPECT_TRUE(fs::is_directory(*program_data));

  const Result<fs::path> local = local_app_data_dir();
  ASSERT_TRUE(local.has_value());
  EXPECT_TRUE(fs::is_directory(*local));

  const Result<fs::path> exe = executable_path();
  ASSERT_TRUE(exe.has_value());
  EXPECT_EQ(exe->filename(), "myclean_platform_service_tests.exe");
}

TEST(SystemTest, ReadsTheOwnerOfAFile) {
  const TempDir dir;
  const fs::path file = dir.path() / "owned.txt";
  std::ofstream(file) << "x";
  const Result<std::string> file_owner = file_owner_sid(file);
  ASSERT_TRUE(file_owner.has_value()) << file_owner.error().message;
  const Result<std::string> user = current_user_sid();
  ASSERT_TRUE(user.has_value());
  // New files belong to the user, or to Administrators when the token is elevated.
  EXPECT_TRUE(*file_owner == *user || *file_owner == administrators_sid) << *file_owner;

  const Result<std::string> missing = file_owner_sid(dir.path() / "missing.txt");
  EXPECT_FALSE(missing.has_value());
}

TEST(SystemTest, OnlyLocalSystemReadsTheConsoleUser) {
  const Result<std::string> user = current_user_sid();
  ASSERT_TRUE(user.has_value());
  if (*user == system_sid) {
    GTEST_SKIP() << "runs as LocalSystem";
  }
  const Result<std::string> console = console_user_sid();
  ASSERT_FALSE(console.has_value());
  EXPECT_TRUE(console.error().code == Errc::access_denied ||
              console.error().code == Errc::not_found)
      << console.error().message;
}

}  // namespace
}  // namespace myclean::platform
