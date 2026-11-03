#include "tray/uninstall_launcher.hpp"

#include <chrono>
#include <filesystem>
#include <stop_token>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::tray {
namespace {

// Only programs that do not exist are named: nothing starts, no window and no message box shows
// (the launcher suppresses the shell's error UI).
TEST(UninstallLauncherTest, AProgramThatDoesNotExistFailsWithoutStartingAnything) {
  const std::filesystem::path missing =
      std::filesystem::temp_directory_path() /
      ("myclean-no-such-uninstaller-" + std::to_string(::GetCurrentProcessId())) / "unins000.exe";
  ASSERT_FALSE(std::filesystem::exists(missing));
  Win32UninstallLauncher launcher(false);

  const Result<void> launched = launcher.launch(missing, "/S", std::chrono::minutes{1}, {});

  ASSERT_FALSE(launched.has_value());
  EXPECT_EQ(launched.error().code, Errc::not_found) << launched.error().message;
  EXPECT_NE(launched.error().message.find("unins000.exe"), std::string::npos);
}

TEST(UninstallLauncherTest, AnEmptyProgramIsRefusedRatherThanOpeningAFolder) {
  Win32UninstallLauncher launcher(false);
  const Result<void> launched = launcher.launch({}, "", std::chrono::minutes{1}, {});
  ASSERT_FALSE(launched.has_value());
  EXPECT_EQ(launched.error().code, Errc::invalid_argument);
}

}  // namespace
}  // namespace myclean::tray
