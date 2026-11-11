#include "platform/backup/win32_backup.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/backup/backup.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::platform {
namespace {

using std::chrono::hours;
using std::chrono::minutes;

WallTime date(int year, unsigned month, unsigned day) {
  return std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} /
                               std::chrono::day{day}};
}

TEST(CimTime, ReadsWmiDatetimesWithTheirUtcOffset) {
  EXPECT_EQ(cim_time(L"20261012143022.000000-180"),
            date(2026, 10, 12) + hours{17} + minutes{30} + std::chrono::seconds{22});
  EXPECT_EQ(cim_time(L"20261012143022.123456+060"),
            date(2026, 10, 12) + hours{13} + minutes{30} + std::chrono::seconds{22});
  EXPECT_EQ(cim_time(L"20261012143022.000000+000"),
            date(2026, 10, 12) + hours{14} + minutes{30} + std::chrono::seconds{22});
  EXPECT_FALSE(cim_time(L"2026-10-12"));
  EXPECT_FALSE(cim_time(L"20261332143022.000000+000"));
}

// Read-only against this machine: the volumes and the installed programs are what every
// Windows has; nothing here starts a tool or changes a setting.
TEST(Win32BackupSystem, ReadsTheVolumesAndInstalledProgramsOfThisMachine) {
  const Result<std::string> sid = current_user_sid();
  ASSERT_TRUE(sid.has_value()) << sid.error().message;
  const Win32BackupSystem system(*sid);
  const Result<std::vector<backup::Volume>> volumes = system.volumes();
  ASSERT_TRUE(volumes.has_value()) << volumes.error().message;
  EXPECT_FALSE(volumes->empty());
  const Result<std::vector<std::string>> programs = system.installed_programs();
  ASSERT_TRUE(programs.has_value()) << programs.error().message;
  EXPECT_FALSE(programs->empty());
  const Result<backup::FileHistoryFacts> history = system.file_history();
  ASSERT_TRUE(history.has_value()) << history.error().message;
  EXPECT_TRUE(history->configured || history->target.empty());
  const Result<std::vector<backup::OneDriveAccount>> accounts = system.onedrive();
  ASSERT_TRUE(accounts.has_value()) << accounts.error().message;
  for (const backup::OneDriveAccount& account : *accounts) {
    EXPECT_FALSE(account.folder.empty());
  }
}

}  // namespace
}  // namespace myclean::platform
