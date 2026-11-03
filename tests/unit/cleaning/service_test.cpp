// The cleaning service over fakes: scans by tier, the tray-only methods, quarantine (own volume,
// overflow, unhealthy target, purge, early purge on a volume short of space, restore), auto-tier
// plans run after every scan, the hourly auto run (busy volume, catch-up) with its manifest, MCP
// proposals, user tasks of the tray and system settings.

#include "core/cleaning/service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
      {"plans.approve", Json{{"id", id}, {"selection", {{"all", true}}}}},
      {"plans.reject", Json{{"id", id}}},
      {"plans.undo", Json{{"id", id}}},
      {"plans.recycle", Json{{"candidates", {{"ids", {1}}}}}},
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