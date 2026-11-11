// The cleaning service over fakes: scans by tier, the tray-only methods, quarantine (own volume,
// overflow, unhealthy target, purge, early purge on a volume short of space, restore), auto-tier
// plans run after every scan, the hourly auto run (busy volume, catch-up) with its manifest, MCP
// proposals, user tasks of the tray and system settings.

#include "core/cleaning/service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cleaning_fixture.hpp"
#include "core/actions/audit_log.hpp"
#include "core/ipc/frame.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "core/safety/deny_list.hpp"
#include "core/startup/impact.hpp"
#include "core/store/database.hpp"
#include "core/user_tasks/task.hpp"
#include "scanall/scanall_fakes.hpp"

namespace myclean::cleaning {
namespace {

using namespace myclean::literals;
using namespace std::chrono_literals;
using std::filesystem::path;
using testing::Json;

class CleaningServiceTest : public testing::CleaningFixture {
 protected:
  // The usual machine: auto, ask, report and native targets on F:.
  void add_targets() {
    fs().add_file(L"F:\\temp\\a.tmp", 1_MiB);
    fs().add_file(L"F:\\temp\\b.tmp", 2_MiB);
    fs().add_file(L"F:\\cache\\s1.bin", 3_MiB);
    fs().add_file(L"F:\\cache\\s2.bin", 4_MiB);
    fs().add_file(L"F:\\logs\\old.log", 5_MiB);
    fs().add_file(L"F:\\pnpm\\store\\v3\\x", 1_MiB);
  }

  // A requested scan, after the first pass of the worker resolved the owner's session.
  void scan() {
    work();
    EXPECT_EQ(call("cleaning.scan"), (Json{{"started", true}}));
    work();
  }

  // The one plan of `kind` whose title contains `text`.
  Json plan_titled(std::string_view text) {
    for (const Json& plan : plans()) {
      if (plan.at("title").get<std::string>().find(text) != std::string::npos) {
        return plan;
      }
    }
    ADD_FAILURE() << "no plan titled " << text;
    return {};
  }

  Json get(std::int64_t plan) { return call("plans.get", Json{{"id", plan}}); }

  static path quarantined(std::wstring_view root, std::int64_t plan, std::int64_t item,
                          std::wstring_view name) {
    return path(root) / ".myclean-quarantine" / std::to_wstring(plan) / std::to_wstring(item) /
           name;
  }

  // A candidate id of F:\big.iso, found by space.top over the fake directory reader.
  std::int64_t big_file_candidate() {
    fs().add_file(L"F:\\big.iso", 5_GiB);
    reader().listings[path(L"F:\\")] = {space::testing::file("big.iso", 5ULL << 30U, 1)};
    EXPECT_EQ(call("space.top", Json{{"path", "f"}, {"refresh", true}}).at("state"), "scanning");
    work_space();
    const Json top = call("space.top", Json{{"path", "F"}, {"n", 5}});
    EXPECT_EQ(top.at("state"), "ready");
    EXPECT_EQ(top.at("path"), "F:\\");
    EXPECT_EQ(top.at("files").size(), 1U);
    return top.at("files")[0].at("id").get<std::int64_t>();
  }

  bool logged(std::int64_t plan, std::string_view event, std::string_view outcome,
              std::string_view reason = {}) {
    return std::ranges::any_of(log(plan), [&](const actions::LogEntry& entry) {
      return entry.event == event && entry.outcome == outcome &&
             (reason.empty() || entry.reason == reason);
    });
  }
};

TEST_F(CleaningServiceTest, ScanPlansAskGroupsDeletesAutoItemsAtOnceAndOnlyShowsReportItems) {
  add_targets();
  start();
  scan();

  const Json shader = plan_titled("shader-caches");
  EXPECT_EQ(shader.at("state"), "awaiting_approval");
  EXPECT_EQ(shader.at("kind"), "cleanup");
  EXPECT_EQ(shader.at("tier"), "ask");
  EXPECT_EQ(shader.at("scope"), "service");
  EXPECT_EQ(shader.at("proposer"), "scheduler");
  EXPECT_EQ(shader.at("items"), 2);
  EXPECT_EQ(shader.at("bytes"), (7_MiB).count());
  const Json native = plan_titled("pnpm store prune");
  EXPECT_EQ(native.at("kind"), "native");
  EXPECT_EQ(native.at("scope"), "user");
  const Json handler = plan_titled("disk cleanup");
  EXPECT_EQ(handler.at("kind"), "handler");
  EXPECT_EQ(handler.at("scope"), "service");
  EXPECT_EQ(handler.at("bytes"), (7_GiB).count());
  // Auto-tier items are deleted at once, without approval; report-tier items only shown.
  const Json automatic = plan_titled("regenerable");
  EXPECT_EQ(automatic.at("tier"), "auto");
  EXPECT_EQ(automatic.at("state"), "verified");
  EXPECT_FALSE(fs().exists(L"F:\\temp\\a.tmp"));
  EXPECT_FALSE(fs().exists(L"F:\\temp\\b.tmp"));
  EXPECT_TRUE(fs().exists(L"F:\\logs\\old.log"));
  EXPECT_EQ(plans().size(), 4U);
  EXPECT_EQ(count("SELECT COUNT(*) FROM finding WHERE kind = 'clean.report'"), 1);

  const Json status = call("cleaning.status");
  EXPECT_EQ(status.at("state"), "idle");
  EXPECT_EQ(status.at("candidates"), 7);
  EXPECT_TRUE(status.at("unavailable").is_null());
  EXPECT_TRUE(status.at("last_completed").is_number());

  // A group whose plan is still open is not proposed again.
  scan();
  EXPECT_EQ(plans().size(), 4U);
}

// The service runs as LocalSystem, and git refuses the owner's repositories to it: the project
// scan asks the owner context's git, which runs as the owner, and without one proposes nothing.
TEST_F(CleaningServiceTest, ProjectArtifactsAreAskedOfTheOwnersGit) {
  pack_files().put(testing::data_dir / "workspaces.toml", "roots = ['F:\\']\n");
  pack_files().put(testing::data_dir / "packs" / "projects" / "pack.toml",
                   R"toml(id = "projects"
version = "1.0.0"
title = "Projects"
description = "A project artifact case."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)toml");
  pack_files().put(testing::data_dir / "packs" / "projects" / "clean.toml", R"toml(
[[rule]]
id       = "target"
category = "dev-artifacts"
tier     = "ask"
scope    = "user"
strategy = "quarantine"
project  = { marker = "Cargo.toml" }
targets  = ['target']
require  = { git_ignored = true, inactive_days = 14 }
)toml");
  fs().add_file(L"F:\\code\\app\\Cargo.toml", 1_KiB);
  fs().add_directory(L"F:\\code\\app\\.git");
  fs().add_file(L"F:\\code\\app\\target\\debug\\app.exe", 3_MiB);
  start();
  scan();
  EXPECT_EQ(count("SELECT COUNT(*) FROM cleaning_candidate WHERE rule = 'projects/target'"), 0);

  clean::testing::FakeGit& git = owner_git();
  git.set_last_commit(L"F:\\code\\app", testing::noon - std::chrono::days{60});
  git.ignore(L"F:\\code\\app\\target");
  start();
  scan();
  const Json plan = plan_titled("dev-artifacts");
  EXPECT_EQ(plan.at("items"), 1);
  EXPECT_EQ(plan.at("bytes"), (3_MiB).count());
}

TEST_F(CleaningServiceTest, PlanGetShowsItemsCommandsAndUndo) {
  add_targets();
  start();
  scan();
  const Json plan = get(plan_titled("shader-caches").at("id").get<std::int64_t>());
  ASSERT_EQ(plan.at("items").size(), 2U);
  EXPECT_EQ(plan.at("items_total"), 2);
  const Json& item = plan.at("items")[0];
  EXPECT_EQ(item.at("path"), "F:\\cache\\s1.bin");
  EXPECT_EQ(item.at("rule"), "test/ask.f");
  EXPECT_NE(item.at("reason").get<std::string>().find("shader-caches"), std::string::npos);
  EXPECT_TRUE(item.at("approved").is_null());
  EXPECT_EQ(plan.at("commands"), Json::array({"move 2 items to the quarantine for 7 days"}));
  EXPECT_FALSE(plan.at("undo").get<std::string>().empty());
  EXPECT_TRUE(plan.at("freed").is_null());
  EXPECT_EQ(error_of("plans.get", Json{{"id", 999}}, false).at("data").at("errc"), "not_found");
}

TEST_F(CleaningServiceTest, TrayOnlyMethodsRefuseEveryOtherCaller) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  const std::vector<std::pair<std::string, Json>> calls = {
      {"cleaning.scan", Json::object()},
      {"plans.approve", Json{{"id", id}, {"selection", {{"all", true}}}}},
      {"plans.reject", Json{{"id", id}}},
      {"plans.undo", Json{{"id", id}}},
      {"plans.recycle", Json{{"candidates", {{"ids", {1}}}}}},
      {"plans.owner_action", Json{{"candidate", 1}}},
      {"user_tasks.next", Json::object()},
      {"user_tasks.report", Json{{"id", id}, {"result", {{"state", "verified"}}}}},
      {"quarantine.restore", Json{{"plan", id}}},
      {"quarantine.purge", Json{{"plan", id}}}};
  for (const auto& [method, params] : calls) {
    const Json error = error_of(method, params, false);
    ASSERT_FALSE(error.is_null()) << method;
    EXPECT_EQ(error.at("code"), ipc::rpc_error::server_error) << method;
    EXPECT_EQ(error.at("data").at("errc"), "refused") << method;
  }
  EXPECT_EQ(get(id).at("state"), "awaiting_approval");
  EXPECT_EQ(count("SELECT COUNT(*) FROM action_log WHERE to_state = 'approved' AND plan_id = " +
                  std::to_string(id)),
            0);
}

TEST_F(CleaningServiceTest, ApprovingSomeItemsQuarantinesOnlyThoseOnTheirVolume) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  const Json items = get(id).at("items");
  const std::int64_t first = items[0].at("id").get<std::int64_t>();
  EXPECT_EQ(
      call("plans.approve", Json{{"id", id}, {"selection", {{"all", false}, {"items", {first}}}}}),
      (Json{{"state", "approved"}}));
  work();

  const Json plan = get(id);
  EXPECT_EQ(plan.at("state"), "verified");
  EXPECT_EQ(plan.at("items")[0].at("approved"), true);
  EXPECT_EQ(plan.at("items")[1].at("approved"), false);
  EXPECT_EQ(plan.at("freed"), 0);  // a rename on its own volume frees nothing until the purge
  EXPECT_FALSE(fs().exists(L"F:\\cache\\s1.bin"));
  EXPECT_TRUE(fs().exists(quarantined(L"F:\\", id, first, L"s1.bin")));
  EXPECT_TRUE(fs().exists(L"F:\\cache\\s2.bin"));

  const Json runs = call("quarantine.list").at("runs");
  ASSERT_EQ(runs.size(), 1U);
  EXPECT_EQ(runs[0].at("plan"), id);
  EXPECT_EQ(
      runs[0].at("purge_after").get<std::int64_t>() - runs[0].at("moved_at").get<std::int64_t>(),
      7 * 24 * 3600);
  EXPECT_EQ(call("quarantine.restore", Json{{"plan", id}}), (Json{{"restored", 1}}));
  EXPECT_TRUE(fs().exists(L"F:\\cache\\s1.bin"));
  EXPECT_EQ(call("quarantine.list").at("runs"), Json::array());
}

TEST_F(CleaningServiceTest, DeleteNowPurgesARunBeforeItsExpiry) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  const std::int64_t first = get(id).at("items")[0].at("id").get<std::int64_t>();
  approve_all(id);
  work();
  ASSERT_TRUE(fs().exists(quarantined(L"F:\\", id, first, L"s1.bin")));

  EXPECT_EQ(call("quarantine.purge", Json{{"plan", id}}), (Json{{"queued", true}}));
  EXPECT_EQ(call("cleaning.status").at("purging"), Json::array({id}));
  work();
  EXPECT_FALSE(fs().exists(quarantined(L"F:\\", id, first, L"s1.bin")));
  EXPECT_EQ(call("cleaning.status").at("purging"), Json::array());
  EXPECT_EQ(call("quarantine.list").at("runs"), Json::array());
  EXPECT_EQ(error_of("quarantine.purge", Json{{"plan", id}}, true).at("data").at("errc"),
            "not_found");
}

TEST_F(CleaningServiceTest, ItemsOfALowSpaceVolumeGoToTheOverflowVolumeAndUndoBringsThemBack) {
  fs().add_file(L"C:\\cache\\big.iso", 4_MiB);  // C: has 8 % free
  start();
  scan();
  const std::int64_t id = plan_titled("c-caches").at("id").get<std::int64_t>();
  const std::int64_t item = get(id).at("items")[0].at("id").get<std::int64_t>();
  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "verified");
  EXPECT_FALSE(fs().exists(L"C:\\cache\\big.iso"));
  EXPECT_TRUE(fs().exists(quarantined(L"L:\\", id, item, L"big.iso")));
  EXPECT_EQ(get(id).at("freed"), (4_MiB).count());  // freed on C: at once

  EXPECT_EQ(call("plans.undo", Json{{"id", id}}),
            (Json{{"state", "rolled_back"}, {"queued", false}}));
  EXPECT_TRUE(fs().exists(L"C:\\cache\\big.iso"));
  EXPECT_FALSE(fs().exists(quarantined(L"L:\\", id, item, L"big.iso")));
}

TEST_F(CleaningServiceTest, AFolderOfALowSpaceVolumeMovesToTheOverflowVolumeAndBack) {
  fs().add_file(L"C:\\cache\\zig\\o\\a.bin", 3_MiB);  // C: has 8 % free
  fs().add_file(L"C:\\cache\\zig\\b.bin", 1_MiB);
  start();
  scan();
  const std::int64_t id = plan_titled("c-caches").at("id").get<std::int64_t>();
  const std::int64_t item = get(id).at("items")[0].at("id").get<std::int64_t>();
  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "verified");
  EXPECT_FALSE(fs().exists(L"C:\\cache\\zig"));
  EXPECT_FALSE(fs().exists(L"C:\\.myclean-quarantine"));
  const path held = quarantined(L"L:\\", id, item, L"zig");
  EXPECT_EQ(fs().stat(held / L"o\\a.bin")->size, 3_MiB);
  EXPECT_EQ(fs().stat(held / L"b.bin")->size, 1_MiB);
  EXPECT_EQ(get(id).at("freed"), (4_MiB).count());  // freed on C: at once

  EXPECT_EQ(call("quarantine.restore", Json{{"plan", id}}), (Json{{"restored", 1}}));
  EXPECT_EQ(fs().stat(L"C:\\cache\\zig\\o\\a.bin")->size, 3_MiB);
  EXPECT_TRUE(fs().exists(L"C:\\cache\\zig\\b.bin"));
  EXPECT_FALSE(fs().exists(held));
}

TEST_F(CleaningServiceTest, AFolderQuarantinedOnTheOverflowVolumeIsPurgedThere) {
  fs().add_file(L"C:\\cache\\zig\\o\\a.bin", 3_MiB);
  start();
  scan();
  const std::int64_t id = plan_titled("c-caches").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  const path run = path(L"L:\\.myclean-quarantine") / std::to_wstring(id);
  ASSERT_TRUE(fs().exists(run));
  EXPECT_EQ(call("quarantine.purge", Json{{"plan", id}}), (Json{{"queued", true}}));
  work();
  EXPECT_FALSE(fs().exists(run));
  EXPECT_FALSE(fs().exists(L"C:\\cache\\zig"));
  EXPECT_EQ(call("quarantine.list").at("runs"), Json::array());
}

TEST_F(CleaningServiceTest, StatusShowsTheProgressOfARunningPlanAndNullOnceDone) {
  add_targets();
  start();
  work();
  EXPECT_TRUE(call("cleaning.status").at("applying").is_null());
  std::vector<Json> seen;  // "applying" as each file is about to be deleted
  fs().on_remove([&](const path&) { seen.push_back(call("cleaning.status").at("applying")); });
  EXPECT_EQ(call("cleaning.scan"), (Json{{"started", true}}));
  work();  // the scan, then its auto-tier plan
  fs().on_remove({});

  ASSERT_EQ(seen.size(), 2U);
  const std::int64_t id = plan_titled("regenerable").at("id").get<std::int64_t>();
  for (const Json& applying : seen) {
    ASSERT_TRUE(applying.is_object());
    EXPECT_EQ(applying.at("kind"), "plan");
    EXPECT_EQ(applying.at("plan"), id);
    EXPECT_EQ(applying.at("title"), "Delete regenerable files");
    EXPECT_EQ(applying.at("items_total"), 2);
    EXPECT_EQ(applying.at("bytes_total"), (3_MiB).count());
    EXPECT_EQ(
        applying.at("started_at"),
        std::chrono::duration_cast<std::chrono::seconds>(testing::noon.time_since_epoch()).count());
    EXPECT_TRUE(applying.at("current").get<std::string>().starts_with("F:\\temp\\"));
  }
  EXPECT_EQ(seen[0].at("items_done"), 0);
  EXPECT_EQ(seen[0].at("bytes_done"), 0);
  EXPECT_EQ(seen[0].at("files_done"), 0);
  EXPECT_EQ(seen[1].at("items_done"), 1);
  EXPECT_EQ(seen[1].at("files_done"), 1);
  EXPECT_GT(seen[1].at("bytes_done").get<std::int64_t>(), 0);
  EXPECT_NE(seen[0].at("current"), seen[1].at("current"));
  EXPECT_TRUE(call("cleaning.status").at("applying").is_null());
}

TEST_F(CleaningServiceTest, StatusShowsTheQuarantineRunBeingDeleted) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  EXPECT_EQ(call("quarantine.purge", Json{{"plan", id}}), (Json{{"queued", true}}));
  std::vector<Json> seen;
  fs().on_remove([&](const path&) { seen.push_back(call("cleaning.status").at("applying")); });
  work();
  fs().on_remove({});
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.front().at("kind"), "purge");
  EXPECT_EQ(seen.front().at("plan"), id);
  EXPECT_EQ(seen.front().at("items_total"), 1);  // the run's folder on F:
  EXPECT_EQ(seen.front().at("bytes_total"), (7_MiB).count());
  EXPECT_EQ(seen.back().at("files_done"), 2);  // both files gone, the folders next
  EXPECT_TRUE(call("cleaning.status").at("applying").is_null());
}

TEST_F(CleaningServiceTest, ItemsAreNeverMovedOntoAnUnhealthyOverflowVolume) {
  fs().add_file(L"C:\\cache\\big.iso", 4_MiB);
  drives().at(L"L:\\").healthy = false;
  drives().at(L"L:\\").health_note = "a new reallocated sector";
  start();
  scan();
  const std::int64_t id = plan_titled("c-caches").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  EXPECT_TRUE(fs().exists(L"C:\\cache\\big.iso"));
  EXPECT_FALSE(fs().exists(L"L:\\.myclean-quarantine"));
  EXPECT_TRUE(logged(id, "apply", "skipped", "quarantine volume health degraded"));
}

// When the volumes cannot be read, the item's own volume may be the full one: nothing is
// quarantined, each item is skipped with the reason, and it stays where it is.
TEST_F(CleaningServiceTest, NothingIsQuarantinedWhileTheVolumesCannotBeRead) {
  fs().add_file(L"C:\\cache\\big.iso", 4_MiB);
  start();
  scan();
  const std::int64_t id = plan_titled("c-caches").at("id").get<std::int64_t>();
  drives().fail_with(Error{.code = Errc::io_error, .message = "WMI did not answer"});
  approve_all(id);
  work();
  EXPECT_TRUE(fs().exists(L"C:\\cache\\big.iso"));
  EXPECT_FALSE(fs().exists(L"C:\\.myclean-quarantine"));
  EXPECT_FALSE(fs().exists(L"L:\\.myclean-quarantine"));
  EXPECT_TRUE(
      logged(id, "apply", "skipped", "free space of the volumes unknown: WMI did not answer"));
}

TEST_F(CleaningServiceTest, HourlyAutoRunDeletesAutoItemsAndWritesItsManifest) {
  add_targets();
  start();
  work();
  EXPECT_TRUE(fs().exists(L"F:\\temp\\a.tmp"));
  const WallTime next_hour = testing::noon + 1h;
  EXPECT_EQ(call("cleaning.status").at("next_auto_run"),
            std::chrono::duration_cast<std::chrono::seconds>(next_hour.time_since_epoch()).count());

  clock().set_wall(next_hour + 10s);
  work();
  EXPECT_FALSE(fs().exists(L"F:\\temp\\a.tmp"));
  EXPECT_FALSE(fs().exists(L"F:\\temp\\b.tmp"));
  EXPECT_TRUE(fs().exists(L"F:\\cache\\s1.bin"));  // ask: waits for the owner
  const Json automatic = plan_titled("regenerable");
  EXPECT_EQ(automatic.at("tier"), "auto");
  EXPECT_EQ(automatic.at("state"), "verified");
  EXPECT_EQ(automatic.at("proposer"), "scheduler");
  bool manifest = false;
  for (const actions::LogEntry& entry : log(automatic.at("id").get<std::int64_t>())) {
    if (entry.event == "manifest") {
      manifest = true;
      EXPECT_EQ(entry.metrics, "freed=" + std::to_string((3_MiB).count()) + " removed=2 skipped=0");
      EXPECT_NE(entry.detail.find("test/auto"), std::string::npos);
    }
  }
  EXPECT_TRUE(manifest);
  EXPECT_EQ(count("SELECT COUNT(*) FROM schedule_run WHERE job = 'cleaning.auto' AND "
                  "status = 'succeeded'"),
            1);
}

TEST_F(CleaningServiceTest, AutoRunWaitsWhileTheSystemVolumeIsCriticallyBusy) {
  add_targets();
  start();
  set_busy(true);
  clock().set_wall(std::chrono::sys_days{std::chrono::November / 1 / 2026} + 3h + 10s);
  work();
  work();
  EXPECT_TRUE(fs().exists(L"F:\\temp\\a.tmp"));
  set_busy(false);
  work();
  EXPECT_FALSE(fs().exists(L"F:\\temp\\a.tmp"));
}

TEST_F(CleaningServiceTest, AMissedRunCatchesUpOnceAfterARestart) {
  add_targets();
  start();
  work();
  clock().advance(std::chrono::days{3});  // the machine was off
  start();
  work();
  EXPECT_FALSE(fs().exists(L"F:\\temp\\a.tmp"));
  work();
  EXPECT_EQ(count("SELECT COUNT(*) FROM schedule_run WHERE job = 'cleaning.auto'"), 1);
  EXPECT_EQ(count("SELECT catch_up FROM schedule_run WHERE job = 'cleaning.auto'"), 1);
}

TEST_F(CleaningServiceTest, TheQuarantineIsPurgedSevenDaysLater) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  const path run = path(L"F:\\.myclean-quarantine") / std::to_wstring(id);
  ASSERT_TRUE(fs().exists(run));
  clock().advance(std::chrono::days{6});
  work();
  EXPECT_TRUE(fs().exists(run));
  clock().advance(std::chrono::days{1} + 1h);
  work();
  EXPECT_FALSE(fs().exists(run));
  EXPECT_EQ(call("quarantine.list").at("runs"), Json::array());
}

TEST_F(CleaningServiceTest, ManyDuplicatesStillFitOnePipeMessage) {
  start();
  work();  // the owner's folders are known once the owner's session is
  // 4000 pairs of equal files with long names: every group cannot be answered at once.
  std::vector<space::ListedEntry> entries;
  for (int group = 0; group < 4000; ++group) {
    for (const char side : {'a', 'b'}) {
      space::ListedEntry entry =
          space::testing::file(std::string(1, side) + "-a-rather-long-photo-name-from-the-camera-" +
                                   std::to_string(group) + ".jpg",
                               2ULL << 20U);
      entry.identity.file_id[0] = static_cast<std::uint8_t>(group & 0xFF);
      entry.identity.file_id[1] = static_cast<std::uint8_t>(group >> 8);
      entry.identity.file_id[2] = static_cast<std::uint8_t>(side);
      entries.push_back(std::move(entry));
    }
  }
  reader().listings[path(L"C:\\Users\\owner\\Pictures")] = std::move(entries);
  EXPECT_EQ(call("space.duplicates", Json{{"refresh", true}}).at("state"), "scanning");
  work_space();

  const Json answer = call("space.duplicates");
  ASSERT_EQ(answer.at("state"), "ready") << answer.dump();
  EXPECT_EQ(answer.at("groups_total"), 4000);
  EXPECT_TRUE(answer.at("truncated").get<bool>());
  EXPECT_GT(answer.at("groups").size(), 100U);
  EXPECT_LT(answer.at("groups").size(), 4000U);
  EXPECT_LT(answer.dump().size(), ipc::max_payload_size);
}

// SPC-5: large files of the owner's folders that nobody wrote for half a year, as a report whose
// rows the owner can send to the Recycle Bin.
TEST_F(CleaningServiceTest, LargeForgottenFilesAreListedWithIdsTheOwnerCanAct) {
  start();
  work();
  const path videos = L"C:\\Users\\owner\\Videos";
  space::ListedEntry recent = space::testing::file("recent.mkv", 2ULL << 30U, 2);
  recent.created = clock().now() - std::chrono::days{10};
  recent.modified = recent.created;
  reader().listings[videos] = {space::testing::file("holiday-2019.mkv", 3ULL << 30U, 1), recent,
                               space::testing::file("small.mp4", 1ULL << 20U, 3)};
  fs().add_file(videos / L"holiday-2019.mkv", 1_MiB);
  EXPECT_EQ(call("space.forgotten_files", Json{{"refresh", true}}, false).at("state"), "scanning");
  work_space();

  const Json answer = call("space.forgotten_files", Json::object(), false);
  ASSERT_EQ(answer.at("state"), "ready") << answer.dump();
  EXPECT_EQ(answer.at("min_size"), 512LL << 20U);
  EXPECT_EQ(answer.at("idle_days"), 180);
  EXPECT_NE(std::ranges::find(answer.at("roots"), Json("C:\\Users\\owner\\Videos")),
            answer.at("roots").end());
  ASSERT_EQ(answer.at("files").size(), 1U);
  const Json& file = answer.at("files")[0];
  EXPECT_EQ(file.at("path"), "C:\\Users\\owner\\Videos\\holiday-2019.mkv");
  EXPECT_EQ(file.at("size_on_disk"), 3LL << 30U);
  EXPECT_GT(file.at("idle_days").get<std::int64_t>(), 180);
  EXPECT_EQ(call("plans.recycle", Json{{"candidates", {{"ids", {file.at("id")}}}}}).at("state"),
            "approved");
}

TEST_F(CleaningServiceTest, PhotoModeGroupsNearDuplicatesWhoseCopiesCanBeProposedOnly) {
  start();
  work();
  const path pictures = L"C:\\Users\\owner\\Pictures";
  for (const wchar_t* name : {L"beach7.jpg", L"beach7_small.jpg", L"city42.jpg", L"broken.jpg"}) {
    fs().add_file(pictures / name, 1_MiB);
  }
  reader().listings[pictures] = {space::testing::file("beach7.jpg", 3ULL << 20U, 1),
                                 space::testing::file("beach7_small.jpg", 1ULL << 20U, 2),
                                 space::testing::file("broken.jpg", 1ULL << 20U, 3),
                                 space::testing::file("city42.jpg", 2ULL << 20U, 4)};
  EXPECT_EQ(error_of("space.duplicates", Json{{"mode", "similar"}}, false).at("data").at("errc"),
            "invalid_argument");
  EXPECT_EQ(
      call("space.duplicates", Json{{"mode", "photos"}, {"refresh", true}}, false).at("state"),
      "scanning");
  work_space();

  const Json answer = call("space.duplicates", Json{{"mode", "photos"}}, false);
  ASSERT_EQ(answer.at("state"), "ready") << answer.dump();
  EXPECT_EQ(answer.at("mode"), "photos");
  EXPECT_EQ(answer.at("analysed"), 3);
  ASSERT_EQ(answer.at("not_analysed").size(), 1U);
  EXPECT_EQ(answer.at("not_analysed")[0].at("path"), "C:\\Users\\owner\\Pictures\\broken.jpg");
  ASSERT_EQ(answer.at("groups").size(), 1U);
  const Json& files = answer.at("groups")[0].at("files");
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[0].at("path"), "C:\\Users\\owner\\Pictures\\beach7.jpg");
  EXPECT_TRUE(files[0].at("keep").get<bool>());
  EXPECT_EQ(files[1].at("path"), "C:\\Users\\owner\\Pictures\\beach7_small.jpg");
  EXPECT_EQ(answer.at("groups")[0].at("distance"), 0);
  EXPECT_EQ(answer.at("reclaimable"), 1ULL << 20U);
  EXPECT_FALSE(answer.at("truncated").get<bool>());
  // The exact-duplicate analysis is a separate result.
  EXPECT_EQ(call("space.duplicates", Json::object(), false).at("state"), "scanning");

  // Nothing was touched; the copy goes only through a proposal the owner approves.
  EXPECT_TRUE(fs().exists(pictures / L"beach7_small.jpg"));
  const Json proposed =
      call("plans.propose_cleanup",
           Json{{"candidates", {{"ids", {files[1].at("id")}}}}, {"client", "claude-code"}}, false);
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  EXPECT_TRUE(fs().exists(pictures / L"beach7_small.jpg"));
}

TEST_F(CleaningServiceTest, TheDevDriveAdviceEstimatesRepositoriesAndCachesAndGuides) {
  pack_files().put(testing::data_dir / "workspaces.toml", "roots = ['F:\\']\n");
  pack_files().put(testing::data_dir / "packs" / "devcaches" / "pack.toml",
                   R"toml(id = "devcaches"
version = "1.0.0"
title = "Dev caches"
description = "A package cache."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)toml");
  pack_files().put(testing::data_dir / "packs" / "devcaches" / "attribution.toml", R"toml(
[[area]]
id       = "npm"
paths    = ['F:\caches\npm']
category = "dev-caches"
label    = "npm cache"
reclaim  = { kind = "relocate", ref = "npm" }
)toml");
  fs().add_file(L"F:\\proj\\.git\\HEAD", 1_KiB);
  fs().add_file(L"F:\\proj\\src\\main.rs", 2_MiB);
  fs().add_file(L"F:\\caches\\npm\\_cacache\\blob", 3_MiB);
  start();
  work();  // the owner's session, then the packs

  EXPECT_EQ(call("space.dev_drive", Json{{"refresh", true}}, false).at("state"), "scanning");
  work_space();
  const Json answer = call("space.dev_drive", Json::object(), false);

  ASSERT_EQ(answer.at("state"), "ready") << answer.dump();
  EXPECT_TRUE(answer.at("supported").get<bool>());
  EXPECT_EQ(answer.at("build"), "26200.7171");
  EXPECT_EQ(answer.at("dev_drives"), Json::array());
  ASSERT_EQ(answer.at("repositories").size(), 1U);
  EXPECT_EQ(answer.at("repositories")[0].at("path"), "F:\\proj");
  ASSERT_EQ(answer.at("caches").size(), 1U);
  EXPECT_EQ(answer.at("caches")[0].at("area"), "devcaches/npm");
  EXPECT_EQ(answer.at("caches")[0].at("relocation"), "npm");
  EXPECT_EQ(answer.at("recommended_bytes"), 50'000'000'000ULL);
  EXPECT_EQ(answer.at("host").at("volume"), "D:");  // the healthy NVMe drive with most room
  EXPECT_FALSE(answer.at("steps").empty());
  EXPECT_FALSE(answer.at("explanation").get<std::string>().empty());
  // Advice only: the files are where they were.
  EXPECT_TRUE(fs().exists(L"F:\\proj\\src\\main.rs"));
  EXPECT_TRUE(fs().exists(L"F:\\caches\\npm\\_cacache\\blob"));
}

// Optimizer spec "VHDX Size Report and Compaction": the report lists each disk with its size; a
// compaction waits for the owner with the warning, and only then stops WSL and compacts.
TEST_F(CleaningServiceTest, VirtualDisksAreCompactedOnlyAfterApprovalAndAfterWslStops) {
  pack_files().put(testing::data_dir / "packs" / "vm" / "pack.toml", R"toml(id = "vm"
version = "1.0.0"
title = "VM disks"
description = "A Docker Desktop disk."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)toml");
  pack_files().put(testing::data_dir / "packs" / "vm" / "attribution.toml", R"toml(
[[area]]
id       = "docker-data"
paths    = ['D:\Docker\wsl']
category = "vm-disks"
label    = "Docker Desktop disk"
reclaim  = { kind = "manual-review", note = "Docker Desktop moves it." }
)toml");
  const path docker = L"D:\\Docker\\wsl\\disk\\docker_data.vhdx";
  fs().add_file(docker, 100_GiB);
  fs().add_file(L"D:\\Docker\\wsl\\main\\ext4.vhdx", 1_GiB);
  start();
  work();

  EXPECT_EQ(call("space.vhdx", Json{{"refresh", true}}, false).at("state"), "scanning");
  work_space();
  const Json report = call("space.vhdx", Json::object(), false);
  ASSERT_EQ(report.at("state"), "ready") << report.dump();
  ASSERT_EQ(report.at("disks").size(), 2U);
  EXPECT_EQ(report.at("disks")[0].at("path"), "D:\\Docker\\wsl\\disk\\docker_data.vhdx");
  EXPECT_EQ(report.at("disks")[0].at("size"), 100ULL << 30U);
  EXPECT_EQ(report.at("disks")[0].at("label"), "Docker Desktop disk");
  EXPECT_EQ(report.at("total"), 101ULL << 30U);
  const std::int64_t disk = report.at("disks")[0].at("id").get<std::int64_t>();

  // A disk is no file to quarantine.
  EXPECT_EQ(error_of("plans.propose_cleanup", Json{{"candidates", {{"ids", {disk}}}}}, false)
                .at("data")
                .at("errc"),
            "invalid_argument");
  const Json proposed = call("plans.propose_compact",
                             Json{{"candidates", {{"ids", {disk}}}}, {"client", "claude"}}, false);
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  const Json plan = get(id);
  EXPECT_EQ(plan.at("kind"), "compact");
  EXPECT_EQ(plan.at("proposer"), "mcp:claude");
  ASSERT_FALSE(plan.at("warnings").empty());
  EXPECT_NE(plan.at("warnings")[0].get<std::string>().find("wsl --shutdown"), std::string::npos);
  work();
  EXPECT_EQ(wsl().shutdowns, 0);  // nothing runs before the owner approves
  EXPECT_TRUE(disks().compacted.empty());

  disks().after[docker] = 60_GiB;
  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "verified");
  EXPECT_EQ(wsl().shutdowns, 1);
  EXPECT_EQ(disks().compacted, (std::vector<path>{docker}));
  const std::vector<actions::LogEntry> entries = log(id);
  const auto verify = std::ranges::find(entries, std::string("verify"), &actions::LogEntry::event);
  ASSERT_NE(verify, entries.end());
  EXPECT_EQ(verify->outcome, "better");
  EXPECT_NE(verify->metrics.find("vhdx_bytes before="), std::string::npos);
  EXPECT_EQ(error_of("plans.undo", Json{{"id", id}}, true).at("data").at("errc"), "not_reversible");
}

TEST_F(CleaningServiceTest, TheHourlyPurgeDeletesAQuarantineOnAVolumeShortOfSpaceAtOnce) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  const path run = path(L"F:\\.myclean-quarantine") / std::to_wstring(id);
  ASSERT_TRUE(fs().exists(run));
  clock().advance(2h);
  work();
  EXPECT_TRUE(fs().exists(run));  // F: has 13 % free: the run keeps its 7 days

  drives().at(L"F:\\").free = Bytes{93ULL << 30U};  // 5 % of 1863 GB
  clock().advance(1h);
  work();
  EXPECT_FALSE(fs().exists(run));
  EXPECT_EQ(call("quarantine.list").at("runs"), Json::array());
  EXPECT_EQ(count("SELECT COUNT(*) FROM action_log WHERE event = 'purge' AND actor = "
                  "'scheduler' AND outcome = 'removed'"),
            1);
}

// A largest file the deny list refuses is listed with the entry that refuses it, so the tray
// shows why instead of a proposal that would fail.
TEST_F(CleaningServiceTest, ALargestFileTheDenyListRefusesSaysWhy) {
  start();
  work();
  fs().add_file(L"F:\\big.iso", 5_GiB);
  fs().add_file(L"F:\\pagefile.sys", 9_GiB);
  reader().listings[path(L"F:\\")] = {space::testing::file("big.iso", 5ULL << 30U, 1),
                                      space::testing::file("pagefile.sys", 9ULL << 30U, 2)};
  call("space.top", Json{{"path", "F"}, {"refresh", true}});
  work_space();
  const Json files = call("space.top", Json{{"path", "F"}}).at("files");
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[0].at("path"), "F:\\pagefile.sys");
  EXPECT_EQ(files[0].at("protected"), "page_file");
  EXPECT_EQ(files[1].at("path"), "F:\\big.iso");
  EXPECT_FALSE(files[1].contains("protected"));
}

TEST_F(CleaningServiceTest, AnMcpProposalWaitsForTheOwnerAndCannotBeApprovedByAnotherCaller) {
  start();
  work();
  const std::int64_t candidate = big_file_candidate();
  const Json proposed =
      call("plans.propose_cleanup",
           Json{{"candidates", {{"ids", {candidate}}}}, {"client", "claude-code"}}, false);
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  EXPECT_EQ(get(id).at("proposer"), "mcp:claude-code");
  EXPECT_TRUE(fs().exists(L"F:\\big.iso"));

  EXPECT_FALSE(
      error_of("plans.approve", Json{{"id", id}, {"selection", {{"all", true}}}}, false).is_null());
  EXPECT_EQ(get(id).at("state"), "awaiting_approval");
  // Only candidate ids: a path does not fit the record, an unknown id is not found.
  EXPECT_EQ(
      error_of("plans.propose_cleanup", Json{{"candidates", {{"ids", {"F:\\big.iso"}}}}}, false)
          .at("code"),
      ipc::rpc_error::invalid_params);
  EXPECT_EQ(error_of("plans.propose_cleanup", Json{{"candidates", {{"ids", {999999}}}}}, false)
                .at("data")
                .at("errc"),
            "not_found");

  // The owner approves it in the tray; the full trail is in the audit log.
  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "verified");
  std::vector<std::string> trail;
  for (const actions::LogEntry& entry : log(id)) {
    if (entry.event == "transition" && entry.outcome == "accepted") {
      trail.push_back(std::string(to_string(*entry.to)) + " " + entry.actor);
    }
  }
  EXPECT_EQ(trail, (std::vector<std::string>{"proposed mcp:claude-code",
                                             "awaiting_approval mcp:claude-code", "approved owner",
                                             "applying owner", "applied owner", "verified owner"}));
  // The tray's own proposal is the owner's.
  const std::int64_t again = big_file_candidate();
  EXPECT_EQ(get(call("plans.propose_cleanup", Json{{"candidates", {{"ids", {again}}}}})
                    .at("plan")
                    .get<std::int64_t>())
                .at("proposer"),
            "owner");
}

TEST_F(CleaningServiceTest, ReportViewDeletionIsARecycleTaskOfTheTray) {
  start();
  work();
  const std::int64_t candidate = big_file_candidate();
  const Json recycled = call("plans.recycle", Json{{"candidates", {{"ids", {candidate}}}}});
  EXPECT_EQ(recycled.at("state"), "approved");
  work();  // the service never runs it
  EXPECT_TRUE(fs().exists(L"F:\\big.iso"));
  const Json task = call("user_tasks.next").at("task");
  EXPECT_EQ(task.at("plan"), recycled.at("plan"));
  EXPECT_EQ(task.at("kind"), "cleanup");
  EXPECT_EQ(task.at("operation"), "apply");
  EXPECT_EQ(task.at("action"), (Json{{"recycle", true}}));
  ASSERT_EQ(task.at("items").size(), 1U);
  EXPECT_EQ(task.at("items")[0].at("path"), "F:\\big.iso");
  EXPECT_EQ(task.at("items")[0].at("file_id").get<std::string>().size(), 32U);

  // The tray reads it as a recycle task, sends the file to the Recycle Bin and reports it.
  const Result<user_tasks::UserTask> parsed = user_tasks::parse_task(task);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->kind, user_tasks::TaskKind::recycle);
  cleanup::testing::FakeRecycler recycler;
  recycler.on_recycle = [&](const std::filesystem::path& file) { fs().erase(file); };
  const safety::ProtectedLocations locations{.windows = LR"(C:\Windows)",
                                             .profiles = LR"(C:\Users)",
                                             .owner_profile = LR"(C:\Users\owner)",
                                             .owner_known_folders = {},
                                             .myclean = {}};
  const user_tasks::TaskReport report = user_tasks::run(
      *parsed,
      user_tasks::Toolbox{.file_system = &fs(), .locations = &locations, .recycler = &recycler},
      std::stop_token{});
  EXPECT_EQ(report.state, PlanState::verified) << report.error;
  EXPECT_EQ(recycler.sent, std::vector<std::filesystem::path>{LR"(F:\big.iso)"});
  EXPECT_FALSE(fs().exists(L"F:\\big.iso"));
  EXPECT_EQ(call("user_tasks.report",
                 Json{{"id", recycled.at("plan")}, {"result", user_tasks::report_record(report)}})
                .at("state"),
            "verified");
  EXPECT_EQ(get(recycled.at("plan").get<std::int64_t>()).at("state"), "verified");
}

// Owner-decisions spec: the cold folders of the owner's profile and the programs in uninstall
// areas reach the tray, and one click is the approval of exactly that plan, run by the tray.
TEST_F(CleaningServiceTest, OwnerDecisionsAreListedAndOneClickApprovesThatPlanForTheTray) {
  pack_files().put(testing::data_dir / "packs" / "test" / "attribution.toml", R"toml(
[[area]]
id       = "games"
paths    = ['?:\Games\*']
category = "games"
label    = "Games"
reclaim  = { kind = "uninstall", note = "Uninstall games you no longer play." }
)toml");
  const std::string key = R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\GameX)";
  placement::testing::FakeRegistry& registry = owner_registry();
  registry.put(key, "DisplayName", placement::RegistryValue{.text = "Game X"});
  registry.put(key, "UninstallString",
               placement::RegistryValue{.text = R"(D:\Games\X\unins.exe /S)"});
  registry.put(key, "InstallLocation", placement::RegistryValue{.text = R"(D:\Games\X)"});
  registry.put(key, "EstimatedSize",
               placement::RegistryValue{.text = "1048576", .type = placement::RegistryType::dword});
  const WallTime old = testing::noon - std::chrono::days{400};
  reader().listings[path(L"C:\\Users\\owner")] = {space::testing::folder("Documents"),
                                                  space::testing::folder("Old")};
  reader().listings[path(L"C:\\Users\\owner\\Documents")] = {
      scanall::testing::file("thesis.bin", 2_GiB, old, 1)};
  reader().listings[path(L"C:\\Users\\owner\\Old")] = {
      scanall::testing::file("scene.bin", 2_GiB, old, 2)};
  fs().add_file(L"C:\\Users\\owner\\Old\\scene.bin", 2_GiB);
  fs().add_file(L"C:\\Users\\owner\\Documents\\thesis.bin", 2_GiB);
  start();
  work();

  EXPECT_EQ(call("space.owner_decisions", Json::object(), false).at("state"), "scanning");
  work_space();
  const Json decisions = call("space.owner_decisions", Json::object(), false);
  ASSERT_EQ(decisions.at("state"), "ready") << decisions.dump();
  EXPECT_EQ(decisions.at("profile"), "C:\\Users\\owner");
  const Json& cold = decisions.at("cold");
  ASSERT_EQ(cold.size(), 2U) << decisions.dump();
  EXPECT_EQ(cold[0].at("path"), "C:\\Users\\owner\\Documents");
  EXPECT_EQ(cold[0].at("protected"), "user_known_folder");
  EXPECT_EQ(cold[1].at("path"), "C:\\Users\\owner\\Old");
  EXPECT_EQ(cold[1].at("target"), "L:\\Archive\\C\\Users\\owner\\Old");
  EXPECT_FALSE(cold[1].contains("protected"));
  const Json& programs = decisions.at("uninstall");
  ASSERT_EQ(programs.size(), 1U) << decisions.dump();
  EXPECT_EQ(programs[0].at("program"), "Game X");
  EXPECT_EQ(programs[0].at("bytes"), (1_GiB).count());
  EXPECT_EQ(programs[0].at("area"), "Games");
  EXPECT_EQ(programs[0].at("reason"), "Games: Uninstall games you no longer play.");
  EXPECT_TRUE(decisions.at("notes").empty()) << decisions.dump();

  // Only the tray acts, and only on an owner decision that is not protected.
  const Json folder{{"candidate", cold[1].at("id")}};
  EXPECT_EQ(error_of("plans.owner_action", folder, false).at("data").at("errc"), "refused");
  EXPECT_EQ(error_of("plans.owner_action", Json{{"candidate", cold[0].at("id")}}, true)
                .at("data")
                .at("errc"),
            "refused");
  EXPECT_EQ(error_of("plans.owner_action", Json{{"candidate", big_file_candidate()}}, true)
                .at("data")
                .at("errc"),
            "invalid_argument");

  const Json archived = call("plans.owner_action", folder);
  EXPECT_EQ(archived.at("state"), "approved");
  const Json plan = get(archived.at("plan").get<std::int64_t>());
  EXPECT_EQ(plan.at("kind"), "archive");
  EXPECT_EQ(plan.at("scope"), "user");
  EXPECT_EQ(plan.at("proposer"), "owner");
  EXPECT_EQ(plan.at("params"), (Json{{"source", "C:\\Users\\owner\\Old"},
                                     {"target", "L:\\Archive\\C\\Users\\owner\\Old"}}));
  const Json uninstalled = call("plans.owner_action", Json{{"candidate", programs[0].at("id")}});
  EXPECT_EQ(uninstalled.at("state"), "approved");
  const Json uninstall = get(uninstalled.at("plan").get<std::int64_t>());
  EXPECT_EQ(uninstall.at("kind"), "uninstall");
  EXPECT_EQ(uninstall.at("title"), "Uninstall Game X");
  EXPECT_EQ(uninstall.at("commands"), Json::array({R"(D:\Games\X\unins.exe /S)"}));

  work();  // the service runs neither
  EXPECT_TRUE(fs().exists(L"C:\\Users\\owner\\Old\\scene.bin"));
  const Json task = call("user_tasks.next").at("task");
  EXPECT_EQ(task.at("kind"), "archive");
  EXPECT_EQ(task.at("action"), plan.at("params"));
  EXPECT_EQ(task.at("items")[0].at("file_id").get<std::string>().size(), 32U);
  const Json next = call("user_tasks.next").at("task");
  EXPECT_EQ(next.at("kind"), "uninstall");
  EXPECT_EQ(next.at("action"), (Json{{"key", key}, {"name", "Game X"}}));

  // A program uninstalled since the list was made gets no plan.
  registry.drop(key);
  EXPECT_EQ(error_of("plans.owner_action", Json{{"candidate", programs[0].at("id")}}, true)
                .at("data")
                .at("errc"),
            "not_found");
}

// Apps spec, "Prompt 4 Answerable Through MCP" and "MCP Proposals Cannot Approve": apps.list
// fuses the Uninstall keys, the Appx packages and the usage sources, and an uninstall proposed by
// an MCP client by its id waits for the tray with the keep reason; nothing is uninstalled.
TEST_F(CleaningServiceTest, InstalledAppsAreListedAndAnUninstallWaitsForTheTray) {
  const std::string root = R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";
  placement::testing::FakeRegistry& registry = owner_registry();
  const auto add = [&](const std::string& subkey, const std::string& name,
                       const std::string& publisher, const std::string& location,
                       const std::string& uninstall) {
    const std::string key = root + "\\" + subkey;
    registry.put(key, "DisplayName", placement::RegistryValue{.text = name});
    registry.put(key, "Publisher", placement::RegistryValue{.text = publisher});
    registry.put(key, "InstallLocation", placement::RegistryValue{.text = location});
    if (!uninstall.empty()) {
      registry.put(key, "UninstallString", placement::RegistryValue{.text = uninstall});
    }
  };
  add("Tool", "Old Tool", "Tools Ltd", R"(C:\Program Files\Old Tool)",
      R"("C:\Program Files\Old Tool\unins000.exe")");
  add("Util", "Board Utility", "ASUSTeK COMPUTER INC.", R"(C:\Program Files\ASUS\Util)",
      R"(C:\Program Files\ASUS\Util\uninstall.exe)");
  add("VC", "Microsoft Visual C++ 2015-2022 Redistributable (x64)", "Microsoft Corporation",
      R"(C:\ProgramData\Package Cache\{X})", R"("C:\ProgramData\Package Cache\{X}\vc.exe" /u)");
  add("Bare", "No Uninstaller", "Bare Inc", R"(C:\Program Files\Bare)", "");
  apps_system().maker = "ASUS";
  apps_system().records = {apps::RunRecord{.source = apps::UsageSource::prefetch,
                                           .path = R"(C:\PROGRAM FILES\OLD TOOL\TOOL.EXE)",
                                           .last_run = testing::noon - std::chrono::days{200}}};
  start();
  work();

  const Json all = call("apps.list", Json::object(), false);
  ASSERT_EQ(all.at("apps").size(), 4U) << all.dump();
  EXPECT_EQ(all.at("manufacturer"), "ASUS");
  const auto app = [&](const Json& list, const std::string& name) {
    for (const Json& entry : list.at("apps")) {
      if (entry.at("name").get<std::string>() == name) {
        return entry;
      }
    }
    return Json();
  };
  EXPECT_EQ(app(all, "Board Utility").at("verdict"), "keep");
  EXPECT_EQ(app(all, "Board Utility").at("reason"), "came with the computer (OEM)");
  const Json runtime = app(all, "Microsoft Visual C++ 2015-2022 Redistributable (x64)");
  EXPECT_EQ(runtime.at("reason"), "runtime or framework");
  const Json tool = app(all, "Old Tool");
  EXPECT_EQ(tool.at("description"), "unknown");
  EXPECT_EQ(tool.at("last_used").at("source"), "Prefetch");
  EXPECT_EQ(tool.at("verdict"), "unknown");
  EXPECT_FALSE(app(all, "No Uninstaller").at("uninstaller").get<bool>());

  const Json unused = call("apps.list", Json{{"unused_days", 180}}, false);
  EXPECT_EQ(unused.at("apps").size(), 4U);  // the tool ran 200 days ago; the others never
  EXPECT_EQ(error_of("apps.list", Json{{"unused_days", 0}}, false).at("data").at("errc"),
            "invalid_argument");

  // Spec "MCP proposal waits for the tray" and "Keep reason travels with the proposal".
  const Json proposed = call("plans.propose_uninstall",
                             Json{{"app", runtime.at("id")}, {"client", "claude-code"}}, false);
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  EXPECT_EQ(proposed.at("verdict"), "keep");
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  const Json plan = get(id);
  EXPECT_EQ(plan.at("proposer"), "mcp:claude-code");
  EXPECT_EQ(plan.at("kind"), "uninstall");
  EXPECT_EQ(plan.at("scope"), "user");
  const std::string vc_uninstall = R"("C:\ProgramData\Package Cache\{X}\vc.exe" /u)";
  EXPECT_EQ(plan.at("commands"), Json::array({vc_uninstall}));
  EXPECT_NE(plan.at("warnings").dump().find("keep: runtime or framework"), std::string::npos)
      << plan.dump();
  EXPECT_FALSE(
      error_of("plans.approve", Json{{"id", id}, {"selection", {{"all", true}}}}, false).is_null());
  EXPECT_EQ(get(id).at("state"), "awaiting_approval");
  EXPECT_TRUE(call("user_tasks.next").at("task").is_null());  // nothing to run before approval

  // Spec "App without an uninstaller": no plan, the reason returned.
  const Json refused = error_of("plans.propose_uninstall",
                                Json{{"app", app(all, "No Uninstaller").at("id")}}, false);
  EXPECT_EQ(refused.at("data").at("errc"), "invalid_argument");
  EXPECT_NE(refused.at("message").get<std::string>().find("no uninstaller registered"),
            std::string::npos);
  EXPECT_EQ(error_of("plans.propose_uninstall", Json{{"app", 99}}, false).at("data").at("errc"),
            "not_found");

  // Once the owner approves it, the tray gets the task naming the registry key MyClean read.
  call("plans.approve", Json{{"id", id}, {"selection", {{"all", true}}}});
  const Json task = call("user_tasks.next").at("task");
  EXPECT_EQ(task.at("kind"), "uninstall");
  EXPECT_EQ(task.at("action"),
            (Json{{"key", root + "\\VC"},
                  {"name", "Microsoft Visual C++ 2015-2022 Redistributable (x64)"}}));
}

TEST_F(CleaningServiceTest, OwnerDecisionsSayWhatCouldNotBeJudged) {
  reader().listings[path(L"C:\\Users\\owner")] = {};
  start();
  work();
  call("space.owner_decisions");
  work_space();
  const Json decisions = call("space.owner_decisions");
  ASSERT_EQ(decisions.at("state"), "ready") << decisions.dump();
  EXPECT_TRUE(decisions.at("cold").empty());
  EXPECT_TRUE(decisions.at("uninstall").empty());
  ASSERT_EQ(decisions.at("notes").size(), 1U);
  EXPECT_NE(decisions.at("notes")[0].get<std::string>().find("registry"), std::string::npos);
}

TEST_F(CleaningServiceTest, UserScopePlansRunInTheTrayAndAreRecordedFromItsReport) {
  add_targets();
  start();
  scan();
  const std::int64_t id = plan_titled("pnpm store prune").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "approved");  // not run by the service

  const Json task = call("user_tasks.next").at("task");
  EXPECT_EQ(task.at("plan"), id);
  EXPECT_EQ(task.at("kind"), "native");
  EXPECT_EQ(task.at("action"), (Json{{"command", "pnpm store prune"}}));
  EXPECT_EQ(call("user_tasks.next"), (Json{{"task", nullptr}}));  // handed out once

  const Json result = {
      {"state", "verified"},
      {"error", nullptr},
      {"not_reversible", "regenerates"},
      {"undo", {{{"item", task.at("items")[0].at("id")}, {"data", "F:\\pnpm"}}}},
      {"verification",
       {{"metric", "cache_bytes"}, {"before", 1048576}, {"after", 0}, {"effect", "better"}}},
      {"skipped", Json::array()}};
  EXPECT_EQ(call("user_tasks.report", Json{{"id", id}, {"result", result}}),
            (Json{{"state", "verified"}}));
  EXPECT_EQ(error_of("plans.undo", Json{{"id", id}}, true).at("data").at("errc"), "not_reversible");
  EXPECT_EQ(error_of("user_tasks.report", Json{{"id", id}, {"result", result}}, true)
                .at("data")
                .at("errc"),
            "invalid_state");
}

TEST_F(CleaningServiceTest, ARelocationIsAppliedAndUndoneByTheTray) {
  start();
  work();
  const Json proposed = call("plans.propose_optimization",
                             Json{{"kind", "relocate"}, {"cache", "npm"}, {"target", "D"}});
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  EXPECT_EQ(get(id).at("kind"), "relocate");
  EXPECT_EQ(get(id).at("proposer"), "owner");
  approve_all(id);
  const Json task = call("user_tasks.next").at("task");
  EXPECT_EQ(task.at("action").at("cache"), "npm");
  EXPECT_EQ(task.at("action").at("target").get<std::string>().substr(0, 3), "D:\\");
  const Json item = task.at("items")[0].at("id");
  // Undo data longer than a record string arrives in pieces.
  EXPECT_EQ(call("user_tasks.report", Json{{"id", id},
                                           {"result",
                                            {{"state", "verified"},
                                             {"undo",
                                              {{{"item", item}, {"data", "first-"}},
                                               {{"item", item}, {"data", "second"}}}}}}}),
            (Json{{"state", "verified"}}));

  EXPECT_EQ(call("plans.undo", Json{{"id", id}}), (Json{{"state", "verified"}, {"queued", true}}));
  const Json undo = call("user_tasks.next").at("task");
  EXPECT_EQ(undo.at("operation"), "undo");
  EXPECT_EQ(undo.at("undo").at("items")[0].at("data"), "first-second");
  EXPECT_EQ(call("user_tasks.report", Json{{"id", id}, {"result", {{"state", "rolled_back"}}}}),
            (Json{{"state", "rolled_back"}}));
}

TEST_F(CleaningServiceTest, SystemSettingsChangeOnlyAfterTheOwnerApproves) {
  system().hibernation_state = {.mode = placement::HibernationMode::full,
                                .file = L"C:\\hiberfil.sys",
                                .file_size = 50_GiB,
                                .memory = 64_GiB};
  tools().on_run = [this](const std::string& command) {
    if (command == "powercfg /h /type reduced") {
      system().hibernation_state.mode = placement::HibernationMode::reduced;
      system().hibernation_state.file_size = 25_GiB;
    }
  };
  start();
  work();
  const Json proposed = call("plans.propose_optimization",
                             Json{{"kind", "hibernation"}, {"client", "claude"}}, false);
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  const Json plan = get(id);
  EXPECT_EQ(plan.at("kind"), "system_setting");
  EXPECT_EQ(plan.at("proposer"), "mcp:claude");
  EXPECT_EQ(plan.at("commands"), Json::array({"powercfg /h /type reduced"}));
  work();
  EXPECT_TRUE(tools().commands.empty());

  approve_all(id);
  work();
  EXPECT_EQ(tools().commands, (std::vector<std::string>{"powercfg /h /type reduced"}));
  EXPECT_EQ(get(id).at("state"), "verified");
  EXPECT_EQ(error_of("plans.propose_optimization", Json{{"kind", "hibernation"}}, true)
                .at("data")
                .at("errc"),
            "invalid_state");  // already reduced
}

// PLC-2: hibernation, the page file and the restore-point storage of C: with their cost and the
// change a proposal would make; reading them changes nothing.
TEST_F(CleaningServiceTest, SystemSpaceStatusShowsEachSettingWithItsCostAndRecommendation) {
  system().hibernation_state = {.mode = placement::HibernationMode::full,
                                .file = L"C:\\hiberfil.sys",
                                .file_size = 50_GiB,
                                .memory = 64_GiB};
  system().page_file_state = {.config = {.automatic = true, .files = {}},
                              .in_use = {{.path = L"C:\\pagefile.sys", .allocated = 16_GiB}}};
  start();
  work();
  const Json first = call("system_space.status").at("settings");
  ASSERT_EQ(first.size(), 3U);
  EXPECT_EQ(first[0].at("kind"), "hibernation");
  EXPECT_EQ(first[0].at("cost"), (50_GiB).count());
  EXPECT_TRUE(first[0].at("recommended").get<std::string>().starts_with("reduced"));
  EXPECT_TRUE(first[0].at("error").is_null());
  EXPECT_EQ(first[1].at("kind"), "page_file");
  EXPECT_EQ(first[1].at("cost"), (16_GiB).count());
  EXPECT_TRUE(first[1].at("restart_required").get<bool>());
  EXPECT_EQ(first[2].at("kind"), "shadow_storage");
  EXPECT_FALSE(first[2].at("error").is_null());  // C: has no shadow storage
  EXPECT_TRUE(first[2].at("cost").is_null());

  system().hibernation_state.mode = placement::HibernationMode::reduced;
  system().shadows = {{.volume = "C:",
                       .diff_volume = "C:",
                       .used = 4_GiB,
                       .allocated = 5_GiB,
                       .maximum = std::nullopt}};
  const Json second = call("system_space.status").at("settings");
  EXPECT_TRUE(second[0].at("recommended").is_null());  // already reduced
  EXPECT_EQ(second[2].at("cost"), (5_GiB).count());
  EXPECT_FALSE(second[2].at("recommended").is_null());  // unbounded: a limit is recommended
  EXPECT_TRUE(tools().commands.empty());
  EXPECT_TRUE(system().written.empty());
}

// The Audit screen: action_log newest first, a page at a time, for the tray only.
TEST_F(CleaningServiceTest, AuditListPagesTheActionLogForTheTrayOnly) {
  system().hibernation_state = {.mode = placement::HibernationMode::full,
                                .file = L"C:\\hiberfil.sys",
                                .file_size = 50_GiB,
                                .memory = 64_GiB};
  start();
  work();
  const std::int64_t id = call("plans.propose_optimization", Json{{"kind", "hibernation"}})
                              .at("plan")
                              .get<std::int64_t>();
  call("plans.reject", Json{{"id", id}});
  const Json first = call("audit.list", Json{{"limit", 1}});
  ASSERT_EQ(first.at("entries").size(), 1U);
  const Json& newest = first.at("entries")[0];
  EXPECT_EQ(newest.at("plan"), id);
  EXPECT_EQ(newest.at("to"), "rejected");
  EXPECT_EQ(newest.at("actor"), "owner");
  ASSERT_TRUE(first.at("next").is_number_integer());
  const Json older = call("audit.list", Json{{"before", first.at("next")}, {"limit", 200}});
  ASSERT_FALSE(older.at("entries").empty());
  EXPECT_LT(older.at("entries")[0].at("id").get<std::int64_t>(),
            newest.at("id").get<std::int64_t>());
  EXPECT_TRUE(older.at("next").is_null());
  EXPECT_EQ(error_of("audit.list", Json::object(), false).at("data").at("errc"), "refused");
  EXPECT_EQ(error_of("audit.list", Json{{"limit", 0}}, true).at("data").at("errc"),
            "invalid_argument");
}

// Startup spec "MCP proposal waits", "Prompt 2 disables only the selection", "Run item
// re-enabled": items are named by their number in startup.list, change only after approval, and
// undo restores them.
TEST_F(CleaningServiceTest, StartupItemsChangeOnlyAfterTheOwnerApprovesAndUndoRestoresThem) {
  startup().snapshot.run = {
      {.scope = startup::Scope::user, .name = "Discord", .command = "Update.exe"},
      {.scope = startup::Scope::user, .name = "Steam", .command = "steam.exe -silent"},
      {.scope = startup::Scope::user, .name = "OneDrive", .command = "OneDrive.exe"}};
  startup().snapshot.services = {
      {.name = "asus", .start = {.type = startup::StartType::automatic, .delayed = true}}};
  start();
  work();
  EXPECT_EQ(error_of("plans.propose_startup", Json{{"candidates", {{"ids", {1}}}}}, true)
                .at("data")
                .at("errc"),
            "not_found");  // no startup.list answer yet
  const Json listed = call("startup.list");
  ASSERT_EQ(listed.at("items").size(), 4U);
  EXPECT_EQ(listed.at("items")[0].at("key"), "run:user:Discord");
  EXPECT_EQ(listed.at("items")[0].at("enabled"), true);
  EXPECT_EQ(listed.at("items")[3].at("key"), "service:machine:asus");
  EXPECT_EQ(listed.at("items")[3].at("delayed"), true);

  const Json proposed = call("plans.propose_startup",
                             Json{{"candidates", {{"ids", {1, 4}}}}, {"client", "claude"}}, false);
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  EXPECT_EQ(get(id).at("kind"), "system_setting");
  EXPECT_EQ(get(id).at("proposer"), "mcp:claude");
  work();
  EXPECT_EQ(startup().writes, 0);

  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "verified");
  EXPECT_TRUE(startup::approved_disabled(
      startup().approved(startup::Scope::user, startup::ApprovedKey::run, "Discord")->bytes));
  EXPECT_EQ(startup().approved(startup::Scope::user, startup::ApprovedKey::run, "Steam"), nullptr);
  EXPECT_EQ(startup().snapshot.services[0].start.type, startup::StartType::manual);
  EXPECT_EQ(startup().snapshot.run.size(), 3U);

  static_cast<void>(call("plans.undo", Json{{"id", id}}));
  work();
  EXPECT_EQ(get(id).at("state"), "rolled_back");
  EXPECT_EQ(startup().approved(startup::Scope::user, startup::ApprovedKey::run, "Discord"),
            nullptr);
  EXPECT_EQ(startup().snapshot.services[0].start,
            (startup::ServiceStart{.type = startup::StartType::automatic, .delayed = true}));
  EXPECT_EQ(error_of("plans.propose_startup", Json{{"candidates", {{"ids", {9}}}}}, true)
                .at("data")
                .at("errc"),
            "not_found");
}

// Optimizer spec "Indexed node_modules detected", "Exclusion undone": a project's node_modules
// inside the indexed scope is listed with an id, excluded only after the owner approves the plan,
// and undo leaves the crawl scope as it was.
TEST_F(CleaningServiceTest,
       IndexedBuildFoldersAreExcludedOnlyAfterApprovalAndUndoRestoresTheScope) {
  pack_files().put(testing::data_dir / "packs" / "node" / "pack.toml",
                   std::string(R"toml(id = "node"
version = "1.0.0"
title = "Node"
description = "Node projects."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)toml"));
  pack_files().put(testing::data_dir / "packs" / "node" / "clean.toml", std::string(R"toml(
[[rule]]
id       = "artifacts"
category = "dev-artifacts"
tier     = "auto"
scope    = "user"
strategy = "delete"
project  = { marker = "package.json" }
targets  = ["node_modules"]
require  = { git_ignored = true, not_in_use = true, inactive_days = 30 }
)toml"));
  pack_files().put(testing::data_dir / "workspaces.toml", std::string("roots = ['F:\\']\n"));
  fs().add_file(LR"(F:\app\package.json)", Bytes{10});
  fs().add_directory(LR"(F:\app\node_modules\react)");
  fs().add_directory(LR"(F:\app\src)");
  indexer().rules_list = {
      indexer::ScopeRule{.url = LR"(file:///F:\)", .included = true, .user = true}};
  const std::vector<indexer::ScopeRule> before = indexer().rules_list;
  start();
  work();

  EXPECT_EQ(call("space.indexed_folders").at("state"), "scanning");
  work_space();
  const Json found = call("space.indexed_folders");
  ASSERT_EQ(found.at("state"), "ready") << found.dump();
  ASSERT_EQ(found.at("folders").size(), 1U) << found.dump();
  const Json& folder = found.at("folders")[0];
  EXPECT_EQ(folder.at("path"), "F:\\app\\node_modules");
  EXPECT_EQ(folder.at("category"), "build-artifacts");
  EXPECT_EQ(folder.at("label"), "Output of projects with package.json");

  const Json proposed =
      call("plans.propose_index_exclusion",
           Json{{"candidates", {{"ids", {folder.at("id")}}}}, {"client", "claude"}}, false);
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  EXPECT_EQ(get(id).at("kind"), "system_setting");
  EXPECT_EQ(get(id).at("proposer"), "mcp:claude");
  work();
  EXPECT_EQ(indexer().writes, 0);
  EXPECT_TRUE(indexer().included(LR"(F:\app\node_modules)").value());

  approve_all(id);
  work();
  EXPECT_EQ(get(id).at("state"), "verified");
  EXPECT_FALSE(indexer().included(LR"(F:\app\node_modules)").value());
  EXPECT_TRUE(indexer().included(LR"(F:\app\src)").value());

  static_cast<void>(call("plans.undo", Json{{"id", id}}));
  work();
  EXPECT_EQ(get(id).at("state"), "rolled_back");
  EXPECT_EQ(indexer().rules_list, before);

  // A candidate of another analysis is no indexed folder.
  EXPECT_EQ(error_of("plans.propose_index_exclusion", Json{{"candidates", {{"ids", {999}}}}}, true)
                .at("data")
                .at("errc"),
            "not_found");
}

// Startup spec "Fallback before history exists": before MyClean has measured a logon, an item
// shows the boot delay Windows attributed to its program in the last 30 days, labelled as such;
// an item Windows did not report shows no impact.
TEST_F(CleaningServiceTest, StartupImpactFallsBackToWindowsBootDelaysBeforeAnyLogonIsMeasured) {
  startup().snapshot.run = {
      {.scope = startup::Scope::user, .name = "OneDrive", .command = "OneDrive.exe /background"},
      {.scope = startup::Scope::user, .name = "Steam", .command = "steam.exe -silent"}};
  diagnostics().xml = {
      startup::testing::delay_event_xml("OneDrive.exe", 2500, "2026-10-30T08:01:00.5000000Z"),
      startup::testing::delay_event_xml("OneDrive.exe", 4000, "2026-10-20T08:01:00Z"),
      // Older than 30 days: no longer counts.
      startup::testing::delay_event_xml("steam.exe", 9000, "2026-09-01T08:01:00Z")};
  start();
  const Json items = call("startup.list").at("items");
  ASSERT_EQ(items.size(), 2U);
  EXPECT_EQ(items[0].at("name"), "OneDrive");
  EXPECT_EQ(items[0].at("impact").at("source"), "diagnostics");
  EXPECT_DOUBLE_EQ(items[0].at("impact").at("delay_seconds").get<double>(), 4.0);
  EXPECT_EQ(items[1].at("name"), "Steam");
  EXPECT_TRUE(items[1].at("impact").is_null());

  // A log the service may not read: no impact, and the list still answers.
  diagnostics().error = Error{.code = Errc::access_denied, .message = "denied"};
  EXPECT_TRUE(call("startup.list").at("items")[0].at("impact").is_null());
}

// Startup spec "Slow logon attributed" and "Effect of disabling measured": the latest logon
// with its difference from the 30-day median and the programs that used the most disk in its
// first 2 minutes, and the logons before and after a startup item disabled through a plan.
TEST_F(CleaningServiceTest, BootTimesAttributeTheLatestLogonAndMeasureADisabledItem) {
  using startup::testing::boot_event_xml;
  startup().snapshot.run = {
      {.scope = startup::Scope::user, .name = "Discord", .command = "Update.exe"},
      {.scope = startup::Scope::user, .name = "Steam", .command = "steam.exe -silent"}};
  // Five boots before the change (2026-10-25..29), each logon 30 s.
  for (int day = 29; day >= 25; --day) {
    diagnostics().xml.push_back(boot_event_xml(std::format("2026-10-{}T08:00:00Z", day), 60'000,
                                               30'000, 25'000, 2'000, 3'000));
  }
  start();
  work();
  EXPECT_TRUE(call("startup.boot_times").at("changes").empty());
  static_cast<void>(call("startup.list"));
  const std::int64_t id = call("plans.propose_startup", Json{{"candidates", {{"ids", {1}}}}}, false)
                              .at("plan")
                              .get<std::int64_t>();
  approve_all(id);
  work();  // Discord is disabled at noon on 2026-10-31
  // Its logon is measured at the next boot: until then the verification is pending.
  ASSERT_EQ(get(id).at("state"), "applied");
  EXPECT_EQ(get(id).at("verification").at("result"), "pending");

  // Two boots after it: 20 s, then a slow 70 s logon that MyClean measured.
  diagnostics().xml.insert(
      diagnostics().xml.begin(),
      {boot_event_xml("2026-11-02T08:00:00Z", 100'000, 30'000, 65'000, 2'000, 3'000),
       boot_event_xml("2026-11-01T08:00:00Z", 50'000, 30'000, 15'000, 2'000, 3'000)});
  const WallTime logon = std::chrono::sys_days{std::chrono::November / 2 / 2026} + 8h + 40s;
  ASSERT_TRUE(startup::record_logon(
      store(), logon,
      std::vector<startup::ImageImpact>{
          {.image = "svchost.exe", .cpu_seconds = 9.0, .io_bytes = 100'000},
          {.image = "steam.exe", .cpu_seconds = 3.0, .io_bytes = 900'000'000},
          {.image = "explorer.exe", .cpu_seconds = 4.0, .io_bytes = 100'000}}));
  clock().set_wall(std::chrono::sys_days{std::chrono::November / 2 / 2026} + 12h);

  const Json times = call("startup.boot_times");
  EXPECT_EQ(times.at("source"), "diagnostics");
  EXPECT_TRUE(times.at("log_error").is_null());
  const Json& latest = times.at("latest");
  EXPECT_EQ(latest.at("start"), store::to_unix_seconds(logon - 40s));
  EXPECT_DOUBLE_EQ(latest.at("logon_seconds").get<double>(), 70.0);
  EXPECT_DOUBLE_EQ(latest.at("boot_seconds").get<double>(), 100.0);
  EXPECT_DOUBLE_EQ(latest.at("logon_difference_seconds").get<double>(), 40.0);
  EXPECT_EQ(times.at("median").at("boots"), 7);
  EXPECT_DOUBLE_EQ(times.at("median").at("logon_seconds").get<double>(), 30.0);
  EXPECT_EQ(latest.at("logon"), store::to_unix_seconds(logon));
  const Json& contributors = latest.at("contributors");
  ASSERT_EQ(contributors.size(), 3U);
  EXPECT_EQ(contributors[0].at("image"), "steam.exe");
  EXPECT_EQ(contributors[0].at("item"),
            (Json{{"name", "Steam"}, {"source", "run"}, {"scope", "user"}}));
  EXPECT_EQ(contributors[1].at("image"), "svchost.exe");  // more CPU than explorer.exe
  EXPECT_TRUE(contributors[1].at("item").is_null());

  const Json& changes = times.at("changes");
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes[0].at("plan"), id);
  EXPECT_EQ(changes[0].at("applied"), store::to_unix_seconds(testing::noon));
  EXPECT_EQ(changes[0].at("items"),
            Json::array({Json{{"name", "Discord"}, {"source", "run"}, {"scope", "user"}}}));
  EXPECT_EQ(changes[0].at("before"), (Json{{"logon_seconds", 30.0}, {"boots", 5}}));
  EXPECT_EQ(changes[0].at("after"), (Json{{"logon_seconds", 45.0}, {"boots", 2}}));

  // The log can no longer be read: the boots recorded are still shown.
  diagnostics().error = Error{.code = Errc::access_denied, .message = "denied"};
  const Json unreadable = call("startup.boot_times");
  EXPECT_EQ(unreadable.at("log_error"), "denied");
  EXPECT_EQ(unreadable.at("latest").at("start"), latest.at("start"));
}

TEST_F(CleaningServiceTest, AConsoleRunNeverAppliesHandlersOrSystemSettings) {
  add_targets();
  start(ServiceSettings{.executor = {.system_changes = false}});
  scan();
  const std::int64_t id = plan_titled("disk cleanup").at("id").get<std::int64_t>();
  approve_all(id);
  work();
  const Json plan = get(id);
  EXPECT_EQ(plan.at("state"), "failed");
  EXPECT_NE(plan.at("detail").get<std::string>().find("console"), std::string::npos);
  EXPECT_TRUE(handlers().purged.empty());
}

TEST_F(CleaningServiceTest, WithoutExecutionScansAndApprovalsWorkButNothingRuns) {
  add_targets();
  start(ServiceSettings{.execute = false});
  scan();
  const std::int64_t id = plan_titled("shader-caches").at("id").get<std::int64_t>();
  approve_all(id);
  clock().set_wall(std::chrono::sys_days{std::chrono::November / 1 / 2026} + 3h + 10s);
  work();
  EXPECT_EQ(get(id).at("state"), "approved");
  EXPECT_TRUE(fs().exists(L"F:\\cache\\s1.bin"));
  EXPECT_TRUE(fs().exists(L"F:\\temp\\a.tmp"));
  const Json status = call("cleaning.status");
  EXPECT_TRUE(status.at("next_auto_run").is_null());
  EXPECT_TRUE(status.at("unavailable").is_string());
}

TEST_F(CleaningServiceTest, CleaningWaitsForTheOwnersSession) {
  add_targets();
  set_owner_available(false);
  start();
  work();
  EXPECT_EQ(call("cleaning.scan"), (Json{{"started", false}}));
  EXPECT_NE(call("cleaning.status").at("unavailable").get<std::string>().find("not logged on"),
            std::string::npos);
  EXPECT_EQ(error_of("plans.propose_cleanup", Json{{"candidates", {{"ids", {1}}}}}, false)
                .at("data")
                .at("errc"),
            "invalid_state");

  set_owner_available(true);
  work();  // retried only after a minute
  EXPECT_FALSE(call("cleaning.status").at("unavailable").is_null());
  clock().advance(CleaningService::owner_retry);
  work();
  EXPECT_TRUE(call("cleaning.status").at("unavailable").is_null());
  scan();
  EXPECT_FALSE(plans().empty());
}

// The knowledge packs (docs/packs.md): the clean cases scans evaluate, each pack's status, and
// the relocation recipes plans may name.
TEST_F(CleaningServiceTest, LoadsThePacksReportsTheirStatusAndOffersTheirRecipes) {
  const path broken = testing::data_dir / "packs" / "broken";
  pack_files().put(broken / "pack.toml", std::string(testing::test_manifest));
  pack_files().put(broken / "clean.toml", "[[rule]]\nid = \"x\"\n");
  pack_files().put(testing::data_dir / "packs" / "tool" / "pack.toml",
                   R"(id = "tool"
version = "1.2.3"
title = "Tool"
description = "A tool."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)");
  pack_files().put(testing::data_dir / "packs" / "tool" / "relocate.toml", R"([[relocate]]
id          = "home"
label       = "Tool home"
locations   = ['%USERPROFILE%\.tool']
current     = { env = "TOOL_HOME" }
target_kind = "env"
content     = "move"
undo        = "restore"
)");
  start();
  work();

  const std::vector<ipc::PackStatus> status = service().pack_status();
  ASSERT_EQ(status.size(), 3U);
  EXPECT_EQ(status[0].id, "broken");
  EXPECT_EQ(status[0].status, "rejected");
  EXPECT_FALSE(status[0].applies);
  const std::string message = status[0].message.value_or("");
  EXPECT_NE(message.find("pack 'broken'"), std::string::npos) << message;
  EXPECT_EQ(status[1].id, "test");
  EXPECT_EQ(status[1].version, "1.0.0");
  EXPECT_EQ(status[1].origin, "builtin");
  EXPECT_TRUE(status[1].applies);
  EXPECT_EQ(status[2].version, "1.2.3");
  const std::vector<std::string> problems = service().problems();
  EXPECT_TRUE(std::ranges::any_of(problems, [](const std::string& problem) {
    return problem.starts_with("packs: pack 'broken'");
  }));

  const Json unknown =
      error_of("plans.propose_optimization",
               Json{{"kind", "relocate"}, {"pack", "tool"}, {"recipe", "none"}}, true);
  ASSERT_TRUE(unknown.is_object());
  EXPECT_NE(unknown.at("message").get<std::string>().find("no relocation recipe \"tool/none\""),
            std::string::npos)
      << unknown;
  const Json plan = call("plans.propose_optimization",
                         Json{{"kind", "relocate"}, {"pack", "tool"}, {"recipe", "home"}});
  EXPECT_EQ(plan.at("state"), "awaiting_approval");
}

}  // namespace
}  // namespace myclean::cleaning