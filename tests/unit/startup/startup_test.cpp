#include "core/startup/startup.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "startup_fakes.hpp"

namespace myclean::startup {
namespace {

using namespace std::chrono_literals;
using testing::disabled_value;
using testing::enabled_value;

TEST(StartupApprovedTest, FirstByteBitZeroMeansDisabled) {
  EXPECT_FALSE(approved_disabled(std::nullopt));
  EXPECT_FALSE(approved_disabled(std::vector<std::uint8_t>{}));
  EXPECT_FALSE(approved_disabled(enabled_value()));
  EXPECT_TRUE(approved_disabled(disabled_value()));
  EXPECT_TRUE(approved_disabled(std::vector<std::uint8_t>{7, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_FALSE(approved_disabled(std::vector<std::uint8_t>{6, 0, 0, 0}));
}

// Spike X6: Task Manager writes 03 00 00 00 + the disable FILETIME, and 02 + zeros to enable.
TEST(StartupApprovedTest, BytesMatchWhatTaskManagerWrites) {
  // 2026-10-31T06:11:48Z; the spike recorded 6C A5 B6 B6 FE 68 DD 01, the same second plus 0.74 s.
  const WallTime when = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 6h + 11min + 48s;
  const std::vector<std::uint8_t> off = approved_bytes(true, when);
  ASSERT_EQ(off.size(), 12U);
  EXPECT_EQ(off[0], 3U);
  EXPECT_EQ(off[1], 0U);
  std::uint64_t filetime = 0;
  for (std::size_t index = 11; index >= 4; --index) {
    filetime = (filetime << 8U) | off[index];
  }
  EXPECT_EQ(filetime, 134379007080000000ULL);
  EXPECT_EQ(off[11], 0x01U);
  EXPECT_EQ(off[10], 0xDDU);
  EXPECT_EQ(approved_bytes(false, when), enabled_value());
}

TEST(StartupItemIdTest, RoundTripsNamesWithColonsAndBackslashes) {
  const std::string id = item_id(Source::run, Scope::user, "EPLTarget\\P0000:x ");
  EXPECT_EQ(id, "run:user:EPLTarget\\P0000:x ");
  const Result<ItemKey> key = parse_item_id(id);
  ASSERT_TRUE(key) << key.error().message;
  EXPECT_EQ(key->source, Source::run);
  EXPECT_EQ(key->scope, Scope::user);
  EXPECT_EQ(key->name, "EPLTarget\\P0000:x ");
  EXPECT_FALSE(parse_item_id("run:user:"));
  EXPECT_FALSE(parse_item_id("bogus:user:x"));
  EXPECT_FALSE(parse_item_id("run:everyone:x"));
  EXPECT_FALSE(parse_item_id("run"));
}

// Spec "All sources listed": a WOW6432Node Run value, a common Startup-folder shortcut, a logon
// task and a third-party auto-start service.
TEST(StartupInventoryTest, ListsEverySource) {
  Snapshot snapshot;
  snapshot.run = {{.scope = Scope::machine,
                   .wow64 = true,
                   .name = "Autodesk Genuine Service ",
                   .command = R"("C:\x\ags.exe")"}};
  snapshot.files = {{.scope = Scope::machine,
                     .file_name = "GoXLR App.lnk",
                     .target = "C:\\GoXLR\\GoXLR App.exe"}};
  snapshot.tasks = {{.path = "\\PowerToys\\Autorun for owner",
                     .logon = true,
                     .command = "C:\\PowerToys\\PowerToys.exe",
                     .signer = "Microsoft Corporation"}};
  snapshot.services = {{.name = "MongoDB",
                        .display_name = "MongoDB Server",
                        .start = {.type = StartType::automatic},
                        .binary = "C:\\mongo\\mongod.exe"}};
  const std::vector<Item> items = inventory(snapshot);
  ASSERT_EQ(items.size(), 4U);
  EXPECT_EQ(items[0].id, "run32:machine:Autodesk Genuine Service ");
  EXPECT_EQ(items[0].enabled, true);
  EXPECT_EQ(items[1].id, "folder:machine:GoXLR App.lnk");
  EXPECT_EQ(items[1].command, "C:\\GoXLR\\GoXLR App.exe");
  EXPECT_EQ(items[2].id, "task:machine:\\PowerToys\\Autorun for owner");
  EXPECT_EQ(items[2].enabled, true);
  EXPECT_EQ(items[3].id, "service:machine:MongoDB");
  EXPECT_EQ(items[3].display_name, "MongoDB Server");
  EXPECT_EQ(items[3].start, (ServiceStart{.type = StartType::automatic}));
}

// Spec "Disabled in Task Manager"; spike X6: the state comes from the item's own hive, leftovers
// are not items, RunOnce has no state.
TEST(StartupInventoryTest, StateComesFromStartupApprovedOfTheOwnHive) {
  Snapshot snapshot;
  snapshot.run = {{.scope = Scope::user, .name = "Discord", .command = "Update.exe"},
                  {.scope = Scope::user, .name = "org.openvpn.client", .command = "ovpn.exe"},
                  {.scope = Scope::user, .once = true, .name = "Uninstall 26", .command = "x"},
                  {.scope = Scope::machine, .name = "SecurityHealth", .command = "s.exe"}};
  snapshot.files = {{.scope = Scope::machine, .file_name = "GoXLR App.lnk", .target = "g.exe"},
                    {.scope = Scope::user, .file_name = "desktop.ini", .target = "d"}};
  snapshot.approved = {
      {.scope = Scope::user, .key = ApprovedKey::run, .name = "Discord", .bytes = disabled_value()},
      {.scope = Scope::user, .key = ApprovedKey::run, .name = "NordVPN", .bytes = disabled_value()},
      {.scope = Scope::machine,
       .key = ApprovedKey::run,
       .name = "SecurityHealth",
       .bytes = {7, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}},
      // HKCU also lists common-folder files; HKLM governs them.
      {.scope = Scope::user,
       .key = ApprovedKey::startup_folder,
       .name = "GoXLR App.lnk",
       .bytes = disabled_value()},
      {.scope = Scope::machine,
       .key = ApprovedKey::startup_folder,
       .name = "GoXLR App.lnk",
       .bytes = enabled_value()},
  };
  const std::vector<Item> items = inventory(snapshot);
  ASSERT_EQ(items.size(), 5U);
  EXPECT_EQ(items[0].id, "run:user:Discord");
  EXPECT_EQ(items[0].enabled, false);
  EXPECT_EQ(items[0].approved, disabled_value());
  EXPECT_EQ(items[1].id, "run:user:org.openvpn.client");
  EXPECT_EQ(items[1].enabled, true);  // no value: enabled
  EXPECT_FALSE(items[1].approved.has_value());
  EXPECT_EQ(items[2].id, "run:machine:SecurityHealth");
  EXPECT_EQ(items[2].enabled, false);  // 07
  EXPECT_EQ(items[3].id, "runonce:user:Uninstall 26");
  EXPECT_FALSE(items[3].enabled.has_value());
  EXPECT_FALSE(toggleable(items[3]));
  EXPECT_EQ(items[4].id, "folder:machine:GoXLR App.lnk");
  EXPECT_EQ(items[4].enabled, true);
  EXPECT_EQ(find_item(items, "run:user:NordVPN"), nullptr);
}

// Spec "Microsoft services excluded"; spike X6: WHQL-signed and unsigned services are listed,
// Windows' own tasks are not, wherever they live.
TEST(StartupInventoryTest, WindowsComponentsAreNotStartupItems) {
  Snapshot snapshot;
  snapshot.services = {
      {.name = "Dhcp", .start = {.type = StartType::automatic}, .signer = "Microsoft Windows"},
      {.name = "AMD External Events Utility",
       .start = {.type = StartType::automatic},
       .signer = "Microsoft Windows Hardware Compatibility Publisher"},
      {.name = "edgeupdate",
       .start = {.type = StartType::automatic, .delayed = true},
       .signer = "Microsoft Corporation"},
      {.name = "postgresql-x64-18", .start = {.type = StartType::automatic}},
      {.name = "Steam Client Service", .start = {.type = StartType::manual}, .signer = "Valve"},
  };
  snapshot.tasks = {
      {.path = R"(\Microsoft\Windows\WindowsUpdate\Scheduled Start)", .boot = true},
      {.path = R"(\Microsoft\Windows\Defrag\ScheduledDefrag)",
       .logon = true,
       .command = R"(C:\Windows\System32\defrag.exe)",
       .signer = "Microsoft Windows"},
      {.path = "\\OneDrive Startup Task-S-1-5-21-1",
       .logon = true,
       .command = R"(C:\Users\o\OneDrive.exe)",
       .signer = "Microsoft Corporation"},
      {.path = "\\Adobe Acrobat Update Task", .command = "AdobeARM.exe", .signer = "Adobe Inc."},
      {.path = "\\ASUS\\Update", .enabled = false, .boot = true, .command = "asus.exe"},
  };
  const std::vector<Item> items = inventory(snapshot);
  std::vector<std::string> ids;
  ids.reserve(items.size());
  for (const Item& item : items) {
    ids.push_back(item.id);
  }
  EXPECT_EQ(ids,
            (std::vector<std::string>{
                "task:machine:\\ASUS\\Update", "task:machine:\\OneDrive Startup Task-S-1-5-21-1",
                "service:machine:AMD External Events Utility", "service:machine:edgeupdate",
                "service:machine:postgresql-x64-18"}));
  EXPECT_EQ(items[0].enabled, false);
  EXPECT_EQ(items[3].start, (ServiceStart{.type = StartType::automatic, .delayed = true}));
}

}  // namespace
}  // namespace myclean::startup
