#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "acceptance_fixture.hpp"
#include "core/startup/startup.hpp"
#include "core/store/database.hpp"

// Prompts 1 to 6 of docs/prd.md section 7, each asked with the MCP tools a client such as Claude
// Code calls for it, against the fixture machine of acceptance_fixture.hpp. Every test checks
// the prompt's pass condition and ends with the audit-log check.
namespace myclean::acceptance {
namespace {

// Whether `text` holds a word that judges a program: only a verdict may say it.
bool judges(const Json& value) {
  const std::string text = value.dump();
  return text.find("dangerous") != std::string::npos || text.find("malicious") != std::string::npos;
}

std::set<std::int64_t> pids_of(const Json& trees) {
  std::set<std::int64_t> pids;
  for (const Json& tree : trees) {
    pids.insert(tree.at("pid").get<std::int64_t>());
  }
  return pids;
}

const Json& named(const Json& list, std::string_view member, std::string_view name) {
  for (const Json& entry : list) {
    if (entry.at(member).get<std::string>() == name) {
      return entry;
    }
  }
  static const Json none = Json::object();
  ADD_FAILURE() << "no entry with " << member << " " << name << " in " << list.dump();
  return none;
}

std::int64_t action_rows(store::Store& store, std::string_view event) {
  std::int64_t count = -1;
  EXPECT_TRUE(store
                  .read([&](store::Database& db) -> Result<void> {
                    Result<store::Statement> query =
                        db.prepare("SELECT COUNT(*) FROM action_log WHERE event = ?1");
                    if (!query) {
                      return tl::unexpected(query.error());
                    }
                    query->bind(1, event);
                    if (Result<bool> row = query->step(); !row) {
                      return tl::unexpected(row.error());
                    }
                    count = query->column_int(0);
                    return {};
                  })
                  .has_value());
  return count;
}

// Prompt 1, health check (MON-5, SEC-1): "List every process above 5 % CPU or memory, explain
// each simply, flag what you don't recognize. Close nothing." Pass: all qualifying trees listed
// with plain description, signer and trust flag; nothing closed.
TEST_F(AcceptanceTest, Prompt1ListsEveryQualifyingTreeExplainedAndFlaggedAndClosesNothing) {
  sample(10s);  // CPU rates need two samples
  const Json snapshot = tool("health_snapshot");
  const Json& trees = snapshot.at("trees");
  // PowerToys at 9 % of the 32 processors, editor.exe at 12.5 % of the memory; small.exe neither.
  EXPECT_EQ(pids_of(trees), (std::set<std::int64_t>{100, 200})) << trees.dump();
  for (const Json& tree : trees) {
    const Json& trust = tree.at("trust");
    ASSERT_TRUE(trust.is_object()) << tree.dump();
    EXPECT_TRUE(trust.at("unrecognized").is_boolean());
    EXPECT_TRUE(trust.at("status").is_string());
    EXPECT_TRUE(trust.at("path").is_string());
    // Explained: the knowledge base's plain description, or flagged with the facts behind it.
    if (trust.at("unrecognized").get<bool>()) {
      EXPECT_FALSE(trust.at("reasons").empty());
    } else {
      EXPECT_TRUE(trust.at("description").is_string()) << tree.dump();
    }
    const Json explained = tool("explain_process", Json{{"pid", tree.at("pid")}});
    EXPECT_EQ(explained.at("tree").at("pid"), tree.at("pid"));
    EXPECT_EQ(explained.at("trust"), trust);
  }
  const Json& known = named(trees, "image", "PowerToys.exe");
  EXPECT_EQ(known.at("trust").at("signer"), "Microsoft Corporation");
  EXPECT_EQ(known.at("trust").at("status"), "signed");
  EXPECT_EQ(known.at("trust").at("unrecognized"), false);
  EXPECT_EQ(known.at("processes"), 2);  // the tree with its child
  const Json explained = tool("explain_process", Json{{"pid", 100}});
  EXPECT_TRUE(explained.at("kb").at("description").is_string());

  const Json& editor = named(trees, "image", "editor.exe");
  EXPECT_EQ(editor.at("trust").at("unrecognized"), true);
  EXPECT_TRUE(editor.at("trust").at("signer").is_null());
  EXPECT_TRUE(editor.at("trust").at("description").is_null());  // said as unknown, never guessed
  EXPECT_EQ(editor.at("trust").at("reasons"),
            (Json{"not-in-knowledge-base", "unsigned", "user-writable-path"}));
  ASSERT_EQ(snapshot.at("unrecognized").size(), 1U);
  EXPECT_EQ(snapshot.at("unrecognized")[0].at("pid"), 200);
  EXPECT_TRUE(tool("explain_process", Json{{"pid", 200}}).at("kb").is_null());

  // The same trees by either order; a flag is a fact, never a verdict.
  for (const char* sort : {"cpu", "memory"}) {
    const Json top =
        tool("top_processes", Json{{"sort", sort}, {"qualifying_only", true}, {"limit", 100}});
    EXPECT_EQ(pids_of(top.at("trees")), (std::set<std::int64_t>{100, 200})) << sort;
  }
  EXPECT_FALSE(judges(snapshot));

  // Nothing closed: every process still runs, and nothing was done.
  sample(1s);
  EXPECT_EQ(tool("top_processes", Json{{"limit", 100}}).at("total_trees"), 3);
  EXPECT_EQ(machine().processes->processes().size(), 5U);
  expect_every_action_approved(store());
  EXPECT_EQ(action_rows(store(), "apply"), 0);
}

// Prompt 2, startup programs (OPT-1): "Show everything that starts automatically, say which I
// need, disable only what I select." Pass: all sources listed with impact; only approved items
// disabled; undo works.
TEST_F(AcceptanceTest, Prompt2ListsEveryStartupSourceWithImpactAndDisablesOnlyApprovedItems) {
  const startup::Snapshot before = startup_system().snapshot;
  const Json items = tool("list_startup").at("items");
  ASSERT_EQ(items.size(), 7U) << items.dump();
  std::set<std::string> sources;
  for (const Json& item : items) {
    sources.insert(item.at("source").get<std::string>());
    // Measured by MyClean over the recorded logon.
    ASSERT_TRUE(item.at("impact").is_object()) << item.dump();
    EXPECT_EQ(item.at("impact").at("source"), "myclean") << item.dump();
    EXPECT_EQ(item.at("impact").at("logons"), 1);
  }
  EXPECT_EQ(sources, (std::set<std::string>{"run", "folder", "task", "service"}));
  EXPECT_DOUBLE_EQ(named(items, "name", "Steam").at("impact").at("cpu_seconds").get<double>(), 6.5);

  // The owner picks Discord, Steam and the OneDrive task; the client proposes them.
  const std::int64_t discord_id = named(items, "name", "Discord").at("id").get<std::int64_t>();
  const std::int64_t steam_id = named(items, "name", "Steam").at("id").get<std::int64_t>();
  const std::int64_t task_id = named(items, "source", "task").at("id").get<std::int64_t>();
  const Json proposed =
      tool("propose_disable_startup", Json{{"item_ids", {discord_id, steam_id, task_id}}});
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  const std::int64_t plan = proposed.at("plan").get<std::int64_t>();
  work();
  EXPECT_EQ(startup_system().writes, 0);  // nothing changes before the tray
  EXPECT_FALSE(other_call("plans.approve", Json{{"id", plan}, {"selection", {{"all", true}}}}));
  EXPECT_EQ(tray("plans.get", Json{{"id", plan}}).at("proposer"), "mcp:claude-code");

  // In the tray the owner approves Discord and the task only.
  const Json planned = tray("plans.get", Json{{"id", plan}});
  ASSERT_EQ(planned.at("items").size(), 3U) << planned.dump();
  std::vector<std::int64_t> chosen;
  for (const Json& item : planned.at("items")) {
    if (item.dump().find("Steam") == std::string::npos) {
      chosen.push_back(item.at("id").get<std::int64_t>());
    }
  }
  ASSERT_EQ(chosen.size(), 2U) << planned.dump();
  static_cast<void>(tray("plans.approve",
                         Json{{"id", plan}, {"selection", {{"all", false}, {"items", chosen}}}}));
  work();
  EXPECT_NE(tray("plans.get", Json{{"id", plan}}).at("state"), "failed");
  const startup::Approved* disabled =
      startup_system().approved(startup::Scope::user, startup::ApprovedKey::run, "Discord");
  ASSERT_NE(disabled, nullptr);
  EXPECT_TRUE(startup::approved_disabled(disabled->bytes));
  EXPECT_FALSE(startup_system().snapshot.tasks[0].enabled);
  // Steam, which the owner did not approve, and every other item are as before.
  EXPECT_EQ(startup_system().approved(startup::Scope::user, startup::ApprovedKey::run, "Steam"),
            nullptr);
  EXPECT_EQ(startup_system().snapshot.services[0].start, before.services[0].start);
  EXPECT_EQ(startup_system().snapshot.run.size(), before.run.size());
  const Json listed = tool("list_startup").at("items");
  EXPECT_EQ(named(listed, "name", "Discord").at("enabled"), false);
  EXPECT_EQ(named(listed, "name", "Steam").at("enabled"), true);
  EXPECT_EQ(named(listed, "name", "Spotify.lnk").at("enabled"), true);

  // Undo, from the tray, restores both exactly.
  static_cast<void>(tray("plans.undo", Json{{"id", plan}}));
  work();
  EXPECT_EQ(tray("plans.get", Json{{"id", plan}}).at("state"), "rolled_back");
  EXPECT_EQ(startup_system().approved(startup::Scope::user, startup::ApprovedKey::run, "Discord"),
            nullptr);
  EXPECT_TRUE(startup_system().snapshot.tasks[0].enabled);
  EXPECT_EQ(named(tool("list_startup").at("items"), "name", "Discord").at("enabled"), true);

  // The changes were made, and only after the owner's approval in the tray.
  EXPECT_GT(action_rows(store(), "transition"), 0);
  expect_every_action_approved(store());
}

// Prompt 3, storage (SPC-1, SPC-4, SPC-5): "Top 20 files and folders, old downloads, duplicate
// photos, installers already used. Sizes. Delete nothing without permission." Pass: all four
// lists with sizes; nothing deleted without approval.
TEST_F(AcceptanceTest, Prompt3GivesTheFourListsWithSizesAndDeletesNothingWithoutApproval) {
  const Json top = analysed("storage_top", Json{{"path", "C"}, {"n", 20}});
  const Json& files = top.at("files");
  ASSERT_EQ(files.size(), 20U) << top.dump();
  EXPECT_EQ(files[0].at("path"), R"(C:\Data\archive25.bin)");
  EXPECT_EQ(files[19].at("path"), R"(C:\Data\archive6.bin)");
  for (std::size_t index = 0; index < files.size(); ++index) {
    EXPECT_GT(files[index].at("size_on_disk").get<std::uint64_t>(), 0U);
    EXPECT_TRUE(files[index].at("id").is_number_integer());
    if (index > 0) {
      EXPECT_GE(files[index - 1].at("size_on_disk").get<std::uint64_t>(),
                files[index].at("size_on_disk").get<std::uint64_t>());
    }
  }
  ASSERT_FALSE(top.at("folders").empty());
  EXPECT_LE(top.at("folders").size(), 20U);
  for (const Json& folder : top.at("folders")) {
    EXPECT_GT(folder.at("size_on_disk").get<std::uint64_t>(), 0U) << folder.dump();
  }
  EXPECT_EQ(named(top.at("folders"), "path", R"(C:\Data)").at("size_on_disk"),
            325ULL << 30U);  // 1 + 2 + ... + 25 GiB

  const Json old = analysed("find_old_downloads");
  ASSERT_EQ(old.at("files").size(), 1U) << old.dump();
  const Json& download = old.at("files")[0];
  EXPECT_EQ(download.at("path"), R"(C:\Users\owner\Downloads\setup-old.zip)");
  EXPECT_EQ(download.at("size_on_disk"), 700ULL << 20U);
  EXPECT_EQ(download.at("age_days"), 300);
  EXPECT_EQ(download.at("origin").at("host_url"), "https://example.com/setup-old.zip");

  const Json photos = analysed("find_duplicates", Json{{"mode", "photos"}});
  const Json* resized = nullptr;
  for (const Json& group : photos.at("groups")) {
    EXPECT_GT(group.at("reclaimable").get<std::uint64_t>(), 0U);
    for (const Json& file : group.at("files")) {
      EXPECT_GT(file.at("size_on_disk").get<std::uint64_t>(), 0U);
      if (file.at("path") == R"(C:\Users\owner\Pictures\beach7_small.jpg)") {
        resized = &file;
      }
    }
  }
  ASSERT_NE(resized, nullptr) << photos.dump();
  EXPECT_FALSE(resized->at("keep").get<bool>());  // the copy with fewer pixels goes
  const Json exact = analysed("find_duplicates");
  ASSERT_EQ(exact.at("groups").size(), 1U) << exact.dump();
  EXPECT_EQ(exact.at("groups")[0].at("reclaimable"), 5ULL << 20U);
  EXPECT_EQ(exact.at("groups")[0].at("files").size(), 2U);

  const Json installers = analysed("find_used_installers");
  ASSERT_EQ(installers.at("used").size(), 2U) << installers.dump();
  for (const Json& used : installers.at("used")) {
    EXPECT_GT(used.at("size_on_disk").get<std::uint64_t>(), 0U);
  }
  EXPECT_EQ(named(installers.at("used"), "kind", "msi").at("installed_as"), "Old Tool");
  EXPECT_EQ(named(installers.at("used"), "kind", "exe").at("installed_as"), "Steam");

  // Proposed for cleanup: a plan that waits for the owner; nothing is moved or deleted.
  const Json proposed =
      tool("propose_cleanup", Json{{"candidate_ids",
                                    {files[0].at("id"), download.at("id"), resized->at("id"),
                                     installers.at("used")[0].at("id")}}});
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  work();
  for (const std::filesystem::path& path :
       {std::filesystem::path(LR"(C:\Data\archive25.bin)"), downloads / "setup-old.zip",
        pictures / "beach7_small.jpg", downloads / "OldTool-1.2.msi",
        downloads / "SteamSetup.exe"}) {
    EXPECT_TRUE(fs().exists(path)) << path.string();
  }
  EXPECT_EQ(action_rows(store(), "apply"), 0);
  expect_every_action_approved(store());
}

// Prompt 4, unused applications (APP-1, APP-2): "Apps not opened in six months, what each does,
// which are safe to uninstall, which came with the PC. I decide." Pass: each app has last-used
// with source, description and verdict.
TEST_F(AcceptanceTest, Prompt4GivesEachUnusedAppItsLastUseWithSourceDescriptionAndVerdict) {
  const Json answer = tool("list_installed_apps", Json{{"unused_days", 180}});
  const Json& apps = answer.at("apps");
  std::set<std::string> names;
  for (const Json& app : apps) {
    names.insert(app.at("name").get<std::string>());
    const Json& used = app.at("last_used");
    EXPECT_TRUE(used.at("confidence").is_string()) << app.dump();
    EXPECT_FALSE(used.at("text").get<std::string>().empty()) << app.dump();
    if (used.at("time").is_null()) {
      // No usage recorded is said as such, with the span of MyClean's history, never as a date.
      EXPECT_TRUE(used.at("source").is_null()) << app.dump();
      EXPECT_TRUE(used.at("text").get<std::string>().starts_with("no usage recorded"));
    } else {
      EXPECT_TRUE(used.at("source").is_string()) << app.dump();
      EXPECT_GE(used.at("days_ago").get<std::int64_t>(), 180);
    }
    EXPECT_FALSE(app.at("description").get<std::string>().empty()) << app.dump();
    const std::string verdict = app.at("verdict").get<std::string>();
    EXPECT_TRUE(verdict == "removable" || verdict == "keep" || verdict == "unknown") << verdict;
    if (verdict != "unknown") {
      EXPECT_FALSE(app.at("reason").get<std::string>().empty()) << app.dump();
    }
  }
  // Game Launcher ran 5 days ago.
  EXPECT_EQ(names, (std::set<std::string>{"Old Tool", "Board Utility",
                                          "Microsoft Visual C++ 2015-2022 Redistributable (x64)"}));
  const Json& tool_app = named(apps, "name", "Old Tool");
  EXPECT_EQ(tool_app.at("last_used").at("source"), "Prefetch");
  EXPECT_EQ(tool_app.at("last_used").at("days_ago"), 200);
  EXPECT_EQ(tool_app.at("description"), "Old Tool, a file converter that nothing else needs.");
  EXPECT_EQ(tool_app.at("verdict"), "removable");
  const Json& board = named(apps, "name", "Board Utility");
  EXPECT_EQ(board.at("last_used").at("source"), "UserAssist");
  EXPECT_EQ(board.at("verdict"), "keep");
  EXPECT_EQ(board.at("reason"), "came with the computer (OEM)");
  EXPECT_EQ(board.at("description"), "unknown");  // not in the knowledge base: never invented
  const Json& runtime = named(apps, "name", "Microsoft Visual C++ 2015-2022 Redistributable (x64)");
  EXPECT_TRUE(runtime.at("last_used").at("time").is_null());
  EXPECT_EQ(runtime.at("reason"), "runtime or framework");

  // The owner decides: an uninstall the client proposes waits for the tray, and nothing runs.
  const Json proposed = tool("propose_uninstall", Json{{"app_id", tool_app.at("id")}});
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  work();
  EXPECT_TRUE(tray("user_tasks.next", Json::object()).at("task").is_null());
  EXPECT_EQ(tool("list_installed_apps").at("apps").size(), 4U);
  EXPECT_EQ(action_rows(store(), "apply"), 0);
  expect_every_action_approved(store());
}

// Prompt 5, intruder scan (SEC-1 to SEC-5): "Unrecognized processes, browser extensions I didn't
// install, programs talking to the internet in the background. Known dangerous? Remove nothing."
// Pass: each item explained; "dangerous" only from a verdict; nothing removed.
TEST_F(AcceptanceTest, Prompt5ExplainsEachItemCallsDangerousOnlyFromAVerdictAndRemovesNothing) {
  sample(10s);
  const Json scan = tool("scan_suspicious");
  ASSERT_FALSE(scan.at("items").empty());
  for (const Json& item : scan.at("items")) {
    const std::string status = item.at("status").get<std::string>();
    EXPECT_FALSE(item.at("summary").get<std::string>().empty()) << item.dump();
    if (status == "known_dangerous") {
      const std::string source = item.at("verdict").at("source").get<std::string>();
      EXPECT_TRUE(source == "Microsoft Defender" || source == "VirusTotal") << item.dump();
    } else {
      EXPECT_TRUE(item.at("verdict").is_null()) << item.dump();
      EXPECT_FALSE(judges(item)) << item.dump();
      EXPECT_TRUE(status == "default" || status == "ok" || status == "suspicious") << status;
    }
    if (status == "suspicious") {
      EXPECT_FALSE(item.at("reasons").empty()) << item.dump();
      EXPECT_TRUE(item.at("summary").get<std::string>().starts_with("suspicious because "));
    }
  }
  const Json& agent_item = named(scan.at("items"), "name", "Agent");
  EXPECT_EQ(agent_item.at("status"), "known_dangerous");
  EXPECT_EQ(agent_item.at("verdict").at("source"), "Microsoft Defender");
  EXPECT_EQ(agent_item.at("verdict").at("threat"), "Trojan:Win32/Agent");
  const Json& updater_item = named(scan.at("items"), "name", "Updater");
  EXPECT_EQ(updater_item.at("status"), "suspicious");
  EXPECT_EQ(updater_item.at("summary"),
            "suspicious because unsigned, runs from a user-writable path, not in the knowledge "
            "base");
  EXPECT_EQ(named(scan.at("items"), "name", "Shell").at("status"), "default");
  EXPECT_EQ(scan.at("counts").at("known_dangerous"), 1);

  const Json extensions = tool("list_browser_extensions").at("extensions");
  ASSERT_EQ(extensions.size(), 3U) << extensions.dump();
  for (const Json& extension : extensions) {
    EXPECT_FALSE(extension.at("name").get<std::string>().empty());
    EXPECT_TRUE(extension.at("origin").is_string());
  }
  const Json& forced = named(extensions, "name", "Corp Search");
  EXPECT_EQ(forced.at("origin"), "policy");
  EXPECT_EQ(forced.at("flag"), "probably not installed by you");
  const Json& toolbar = named(extensions, "name", "Shopping Toolbar");
  EXPECT_EQ(toolbar.at("origin"), "external");
  EXPECT_EQ(toolbar.at("flag"), "probably not installed by you");
  const Json& store_extension = named(extensions, "name", "Password Manager");
  EXPECT_EQ(store_extension.at("origin"), "store");
  EXPECT_TRUE(store_extension.at("flag").is_null());
  EXPECT_FALSE(judges(extensions));

  const Json network = tool("list_network_processes");
  const Json& talkers = network.at("processes");
  EXPECT_EQ(pids_of(talkers), (std::set<std::int64_t>{100, 200})) << network.dump();
  for (const Json& talker : talkers) {
    EXPECT_FALSE(talker.at("image").get<std::string>().empty());
    ASSERT_TRUE(talker.at("trust").is_object()) << talker.dump();
    EXPECT_TRUE(talker.at("visible_window").is_null());  // the tray has not reported windows
  }
  const Json& editor = named(talkers, "image", R"(C:\Users\owner\AppData\Local\Editor\editor.exe)");
  EXPECT_EQ(editor.at("trust").at("unrecognized"), true);
  // A background talker over time: six hours with traffic from MyClean's network history.
  EXPECT_EQ(editor.at("active_minutes"), 6);
  EXPECT_EQ(editor.at("sent"), 6 * 20'000);
  EXPECT_EQ(editor.at("first_seen"), store::to_unix_seconds(now - 6h));
  ASSERT_EQ(editor.at("listening").size(), 1U);
  EXPECT_EQ(editor.at("listening")[0].at("port"), 8080);
  EXPECT_EQ(named(editor.at("remotes"), "address", "198.51.100.7").at("port"), 8443);
  EXPECT_FALSE(judges(network));

  // Nothing removed, disabled or scanned: the startup items, Defender and VirusTotal untouched.
  EXPECT_EQ(startup_system().writes, 0);
  EXPECT_EQ(defender_scans(), 0);
  EXPECT_EQ(virustotal_lookups(), 0);
  EXPECT_EQ(tool("scan_suspicious").at("items").size(), scan.at("items").size());
  EXPECT_EQ(tool("list_browser_extensions").at("extensions").size(), 3U);
  expect_every_action_approved(store());
  EXPECT_EQ(action_rows(store(), "apply"), 0);
}

// Prompt 6, backups (BAK-1, BAK-2): "Is there any backup, where, and when was the last one? If
// none, explain the simplest setup with what I have." Pass: all sources checked; last-backup
// age; concrete recommendation.
TEST_F(AcceptanceTest, Prompt6ChecksEveryBackupSourceGivesTheAgeAndRecommendsWithTheOwnersDrives) {
  Json answer = analysed("check_backups");
  std::set<std::string> kinds;
  for (const Json& source : answer.at("sources")) {
    kinds.insert(source.at("source").get<std::string>());
    const std::string status = source.at("status").get<std::string>();
    EXPECT_TRUE(status == "found" || status == "not_found") << source.dump();
  }
  EXPECT_EQ(kinds,
            (std::set<std::string>{"file_history", "system_image", "restore_points",
                                   "shadow_copies", "onedrive", "third_party", "backup_drives"}));
  // Restore points are listed, but live on the drive they protect: no backup exists.
  const Json& points = named(answer.at("sources"), "source", "restore_points");
  EXPECT_EQ(points.at("status"), "found");
  EXPECT_FALSE(points.at("counts_toward_age").get<bool>());
  EXPECT_FALSE(answer.at("backup_exists").get<bool>());
  EXPECT_TRUE(answer.at("last_backup").is_null());
  EXPECT_EQ(answer.at("summary"), "no backup exists");
  // The simplest setup, with the owner's own healthy drives.
  const Json& recommendation = answer.at("recommendation");
  ASSERT_TRUE(recommendation.is_object()) << answer.dump();
  EXPECT_EQ(recommendation.at("drives"), (Json{"L:", "K:"}));
  const std::string text = recommendation.at("text").get<std::string>();
  EXPECT_NE(text.find("File History"), std::string::npos) << text;
  EXPECT_NE(text.find("L:"), std::string::npos) << text;
  // Code that exists only on this disk.
  const Json& repositories = answer.at("repositories");
  EXPECT_EQ(repositories.at("checked"), 3);
  ASSERT_EQ(repositories.at("unprotected").size(), 2U) << repositories.dump();
  EXPECT_EQ(repositories.at("unprotected")[0].at("path"), R"(F:\app)");
  EXPECT_TRUE(repositories.at("unprotected")[0].at("no_remote").get<bool>());
  EXPECT_EQ(repositories.at("unprotected")[1].at("path"), R"(F:\lib)");
  EXPECT_EQ(repositories.at("unprotected")[1].at("unpushed"), 3);
  EXPECT_TRUE(repositories.at("unprotected")[1].at("uncommitted").get<bool>());

  // Once File History copies to K:, the answer says where and how old the last backup is.
  backup_state().history = backup::FileHistoryFacts{
      .configured = true, .target = "K:", .newest = now - std::chrono::days{9}};
  run_backup_check();
  answer = tool("check_backups");
  EXPECT_TRUE(answer.at("backup_exists").get<bool>());
  EXPECT_EQ(answer.at("last_backup").at("source"), "file_history");
  EXPECT_EQ(answer.at("last_backup").at("location"), "K:");
  EXPECT_EQ(answer.at("last_backup").at("age_days"), 9);
  EXPECT_TRUE(answer.at("recommendation").is_null());
  expect_every_action_approved(store());
  EXPECT_EQ(action_rows(store(), "apply"), 0);
}

}  // namespace
}  // namespace myclean::acceptance
