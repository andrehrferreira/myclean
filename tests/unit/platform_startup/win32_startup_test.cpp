// Win32StartupSystem against this machine. Every source is only read; the one write is a
// throwaway HKCU StartupApproved\Run value named MyCleanTestStartup<random>, with no Run value
// of that name (so nothing can start), deleted by the test and verified absent afterwards. No
// task, service, Run value or HKLM key is changed: the task and service writers are only called
// on names that do not exist.

#include "platform/startup/win32_startup.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "core/startup/startup.hpp"
#include "platform/ipc/security.hpp"
#include "platform/settings/registry.hpp"

namespace myclean::platform {
namespace {

using startup::ApprovedKey;
using startup::Scope;
using startup::Snapshot;

constexpr std::wstring_view approved_run =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)";

std::string random_suffix() {
  std::random_device random;
  return std::format("{:08x}{:08x}", random(), random());
}

// The Run or RunOnce key a value was read from, as Win32Registry names it.
std::string run_key_of(const startup::RunValue& value) {
  std::string key = value.scope == Scope::user ? R"(HKCU\Software\)" : R"(HKLM\Software\)";
  if (value.wow64) {
    key += R"(WOW6432Node\)";
  }
  key += R"(Microsoft\Windows\CurrentVersion\)";
  key += value.once ? "RunOnce" : "Run";
  return key;
}

// The bytes of HKCU StartupApproved\Run value `name`; nullopt when there is none.
std::optional<std::vector<std::uint8_t>> approved_run_value(const std::wstring& name) {
  std::vector<std::uint8_t> bytes(64);
  auto size = static_cast<DWORD>(bytes.size());
  const LSTATUS status =
      ::RegGetValueW(HKEY_CURRENT_USER, std::wstring(approved_run).c_str(), name.c_str(),
                     RRF_RT_REG_BINARY | RRF_SUBKEY_WOW6464KEY, nullptr, bytes.data(), &size);
  if (status != ERROR_SUCCESS) {
    return std::nullopt;
  }
  bytes.resize(size);
  return bytes;
}

// Deletes the throwaway StartupApproved value however the test ends.
class ThrowawayApproved {
 public:
  ThrowawayApproved()
      : name_("MyCleanTestStartup" + random_suffix()), wide_(name_.begin(), name_.end()) {}
  ThrowawayApproved(const ThrowawayApproved&) = delete;
  ThrowawayApproved& operator=(const ThrowawayApproved&) = delete;
  ThrowawayApproved(ThrowawayApproved&&) = delete;
  ThrowawayApproved& operator=(ThrowawayApproved&&) = delete;
  ~ThrowawayApproved() {
    ::RegDeleteKeyValueW(HKEY_CURRENT_USER, std::wstring(approved_run).c_str(), wide_.c_str());
  }

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::wstring& wide() const { return wide_; }

 private:
  std::string name_;  // ASCII
  std::wstring wide_;
};

TEST(Win32StartupSystem, ReadsTasksServicesAndFilesOfThisMachine) {
  const Win32StartupSystem system;
  const Result<Snapshot> snapshot = system.read();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message;

  EXPECT_FALSE(snapshot->tasks.empty());
  for (const startup::Task& task : snapshot->tasks) {
    EXPECT_TRUE(task.path.starts_with('\\')) << task.path;
  }
  ASSERT_FALSE(snapshot->services.empty());
  // Most automatic services are Windows' own: their signer is found through the catalogs.
  EXPECT_TRUE(std::ranges::any_of(snapshot->services, [](const startup::Service& service) {
    return service.start.type == startup::StartType::automatic && service.signer.has_value();
  }));
  for (const startup::Service& service : snapshot->services) {
    EXPECT_FALSE(service.name.empty());
    if (service.start.type != startup::StartType::automatic) {
      EXPECT_EQ(service.signer, std::nullopt) << service.name;
    }
  }
  for (const startup::StartupFile& file : snapshot->files) {
    EXPECT_FALSE(file.file_name.empty());
    EXPECT_EQ(file.file_name.find('\\'), std::string::npos) << file.file_name;
    EXPECT_FALSE(file.target.empty()) << file.file_name;
  }
  std::set<std::tuple<Scope, ApprovedKey, std::string>> approved;
  for (const startup::Approved& value : snapshot->approved) {
    EXPECT_TRUE(approved.emplace(value.scope, value.key, value.name).second) << value.name;
  }
}

TEST(Win32StartupSystem, RunValueNamesAndCommandsReadBackExactly) {
  const Win32StartupSystem system;
  const Result<Snapshot> snapshot = system.read();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message;
  const Win32Registry registry;
  for (const startup::RunValue& value : snapshot->run) {
    const std::string key = run_key_of(value);
    const Result<std::optional<placement::RegistryValue>> stored = registry.read(key, value.name);
    ASSERT_TRUE(stored.has_value()) << stored.error().message;
    ASSERT_TRUE(stored->has_value()) << key << " \"" << value.name << "\"";
    EXPECT_EQ(stored->value_or(placement::RegistryValue{}).text, value.command) << value.name;
  }
}

TEST(Win32StartupSystem, TheOwnersSidReadsTheSameUserItemsAsThisProcess) {
  const Result<std::string> sid = current_user_sid();
  ASSERT_TRUE(sid.has_value()) << sid.error().message;
  const Result<Snapshot> own = Win32StartupSystem{}.read();
  const Result<Snapshot> by_sid = Win32StartupSystem{*sid}.read();
  ASSERT_TRUE(own.has_value()) << own.error().message;
  ASSERT_TRUE(by_sid.has_value()) << by_sid.error().message;

  const auto user_run = [](const Snapshot& snapshot) {
    std::set<std::tuple<bool, bool, std::string, std::string>> run;
    for (const startup::RunValue& value : snapshot.run) {
      if (value.scope == Scope::user) {
        run.emplace(value.wow64, value.once, value.name, value.command);
      }
    }
    return run;
  };
  const auto user_files = [](const Snapshot& snapshot) {
    std::set<std::string> files;
    for (const startup::StartupFile& file : snapshot.files) {
      if (file.scope == Scope::user) {
        files.insert(file.file_name);
      }
    }
    return files;
  };
  EXPECT_EQ(user_run(*by_sid), user_run(*own));
  EXPECT_EQ(user_files(*by_sid), user_files(*own));
}

TEST(Win32StartupSystem, WritesAndDeletesAThrowawayApprovedValueWithNoRunValue) {
  const ThrowawayApproved value;
  const Win32Registry registry;
  const Result<std::optional<placement::RegistryValue>> run =
      registry.read(R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)", value.name());
  ASSERT_TRUE(run.has_value()) << run.error().message;
  ASSERT_FALSE(run->has_value());  // nothing of that name can start
  ASSERT_EQ(approved_run_value(value.wide()), std::nullopt);

  Win32StartupSystem system;
  const std::vector<std::uint8_t> disabled =
      startup::approved_bytes(true, std::chrono::system_clock::now());
  ASSERT_TRUE(
      system.set_approved(Scope::user, ApprovedKey::run, value.name(), disabled).has_value());
  EXPECT_EQ(approved_run_value(value.wide()), disabled);

  const Result<Snapshot> snapshot = system.read();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message;
  EXPECT_TRUE(std::ranges::any_of(snapshot->approved, [&](const startup::Approved& approved) {
    return approved.scope == Scope::user && approved.key == ApprovedKey::run &&
           approved.name == value.name() && approved.bytes == disabled;
  }));
  // A leftover value with no Run value is not an item.
  EXPECT_EQ(startup::find_item(startup::inventory(*snapshot),
                               startup::item_id(startup::Source::run, Scope::user, value.name())),
            nullptr);

  const std::vector<std::uint8_t> enabled = startup::approved_bytes(false, WallTime{});
  ASSERT_TRUE(
      system.set_approved(Scope::user, ApprovedKey::run, value.name(), enabled).has_value());
  EXPECT_EQ(approved_run_value(value.wide()), enabled);

  ASSERT_TRUE(
      system.set_approved(Scope::user, ApprovedKey::run, value.name(), std::nullopt).has_value());
  EXPECT_EQ(approved_run_value(value.wide()), std::nullopt);
  EXPECT_TRUE(  // deleting a missing value succeeds
      system.set_approved(Scope::user, ApprovedKey::run, value.name(), std::nullopt).has_value());
  EXPECT_EQ(approved_run_value(value.wide()), std::nullopt);
}

TEST(Win32StartupSystem, TaskAndServiceWritersReportAMissingTarget) {
  Win32StartupSystem system;
  const std::string missing = "MyCleanTestMissing" + random_suffix();
  const Result<void> task = system.set_task_enabled("\\" + missing, true);
  ASSERT_FALSE(task.has_value());
  EXPECT_EQ(task.error().code, Errc::not_found) << task.error().message;
  const Result<void> service =
      system.set_service_start(missing, startup::ServiceStart{.type = startup::StartType::manual});
  ASSERT_FALSE(service.has_value());
  EXPECT_EQ(service.error().code, Errc::not_found) << service.error().message;
}

}  // namespace
}  // namespace myclean::platform
