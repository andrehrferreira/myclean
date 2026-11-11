#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cleaning/cleaning_fixture.hpp"
#include "core/actions/schema.hpp"
#include "core/apps/apps.hpp"
#include "core/backup/backup.hpp"
#include "core/backup/repos.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleaning/service.hpp"
#include "core/elimination/pauses.hpp"
#include "core/events/connections.hpp"
#include "core/events/event_store.hpp"
#include "core/events/events.hpp"
#include "core/input/incident_store.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/mcp/client.hpp"
#include "core/mcp/server.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/loader.hpp"
#include "core/placement/drives.hpp"
#include "core/report/repository.hpp"
#include "core/resident/history.hpp"
#include "core/security/extensions.hpp"
#include "core/security/network.hpp"
#include "core/security/persistence.hpp"
#include "core/security/verdicts.hpp"
#include "core/space/downloads.hpp"
#include "core/space/installers.hpp"
#include "core/space/scanner.hpp"
#include "core/startup/impact.hpp"
#include "core/startup/startup.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/trust/trust.hpp"
#include "events/event_fakes.hpp"
#include "fakes/fake_clock.hpp"
#include "rules/fake_data_files.hpp"
#include "svc/backup_component.hpp"
#include "svc/component.hpp"
#include "svc/phase1_fakes.hpp"
#include "svc/report_component.hpp"
#include "svc/security_component.hpp"
#include "svc/telemetry_component.hpp"
#include "svc/trust_component.hpp"

// The fixture machine of the seven-prompt acceptance suite (docs/acceptance.md): one store with
// every component's tables and one dispatcher with every component's methods, as in myclean-svc,
// over the fakes of the unit tests. The MCP server answers the prompts' tool calls through
// Server::handle; the tray's calls (approve, undo) go to the same dispatcher as the tray caller.
//
// The machine, 2026-10-31 12:00 UTC:
//   processes  the fake machine of tests/unit/svc (32 logical processors, 64 GiB): PowerToys.exe
//              (pid 100 with child 101, signed by Microsoft, a knowledge-base entry, 9 % CPU),
//              editor.exe (pid 200, unsigned in the owner's AppData, 12.5 % of memory), small.exe
//              (pid 300, below 5 %, its image unreadable)
//   startup    Run values Discord, Steam, Updater (unsigned, %TEMP%) and Agent (a Defender
//              detection), Startup-folder file Spotify.lnk, logon task OneDrive, auto service
//              ArmouryCrateService; one logon measured by MyClean
//   files      C:\ with 25 large files in C:\Data, the owner's Downloads (an old download, two
//              installers already installed, a recent file), Pictures (a resized copy of a photo
//              and an exact duplicate)
//   apps       Old Tool (Prefetch, 200 days, a knowledge-base entry safe to remove), Board
//              Utility (the maker's, UserAssist 400 days), the Visual C++ runtime (no usage),
//              Game Launcher (BAM, 5 days)
//   security   Edge with a store extension and a policy-forced one, Chrome with an extension a
//              program installed; PowerToys and editor.exe talking to the internet, editor.exe
//              listening on 8080
//   backups    restore points only (no backup), K: "Backup 1" and L: "Backup 2" healthy HDDs,
//              repositories F:\app (no remote), F:\lib (unpushed and uncommitted), F:\ok
//   reports    no snapshot yet; the monthly report's scheduler is polled by the test
namespace myclean::acceptance {

using Json = nlohmann::json;
using namespace std::chrono_literals;

inline const WallTime now = cleaning::testing::noon;
inline constexpr std::string_view owner_sid = "S-1-5-21-1";
inline constexpr std::string_view client_name = "claude-code";

inline const std::filesystem::path downloads = LR"(C:\Users\owner\Downloads)";
inline const std::filesystem::path pictures = LR"(C:\Users\owner\Pictures)";

// Program images of the machine.
inline const std::filesystem::path discord = LR"(C:\Users\owner\AppData\Local\Discord\Update.exe)";
inline const std::filesystem::path steam = LR"(C:\Program Files (x86)\Steam\steam.exe)";
inline const std::filesystem::path updater = LR"(C:\Users\owner\AppData\Local\Temp\upd.exe)";
inline const std::filesystem::path agent = LR"(C:\Users\owner\AppData\Roaming\x\agent.exe)";
inline const std::filesystem::path spotify =
    LR"(C:\Users\owner\AppData\Roaming\Spotify\Spotify.exe)";
inline const std::filesystem::path onedrive =
    LR"(C:\Program Files\Microsoft OneDrive\OneDrive.exe)";
inline const std::filesystem::path armoury =
    LR"(C:\Program Files\ASUS\ArmouryCrate\ArmouryCrate.Service.exe)";
// Programs that start automatically only once the prompt-7 test installs them.
inline const std::filesystem::path teams = LR"(C:\Program Files\Teams\ms-teams.exe)";
inline const std::filesystem::path backup_agent = LR"(C:\Program Files\Backup Co\agent-svc.exe)";

inline std::string quoted(const std::filesystem::path& path) {
  return '"' + path.string() + '"';
}

// ---- The audit-log check -------------------------------------------------------------------

// The action_log rows that act on the machine without the owner's approval of their plan in the
// tray. Bookkeeping is no action: a plan proposed or put to the owner (a transition to proposed
// or awaiting_approval), a transition the policy engine refused, the owner's approval or
// rejection and the owner's item decisions. Every other row (a plan approved, applying, applied,
// verified, rolled back or failed, an item applied, restored or purged, a governor step, a pause, a
// manifest) needs an earlier accepted transition of its plan to approved by the owner, which only
// the tray's plans.approve makes. Each is named "plan <id>: <event> <to or outcome> by <actor> (row
// <id>)".
inline std::vector<std::string> unapproved_actions(store::Store& store) {
  std::vector<std::string> found;
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(R"sql(
SELECT a.id, a.plan_id, a.event, COALESCE(a.to_state, ''), a.outcome, a.actor
FROM action_log a
WHERE NOT (a.event = 'transition' AND (a.outcome = 'rejected' OR
                                       a.to_state IN ('proposed', 'awaiting_approval') OR
                                       (a.to_state IN ('approved', 'rejected') AND
                                        a.actor = 'owner')))
  AND NOT (a.event = 'approval' AND a.actor = 'owner')
  AND NOT EXISTS (SELECT 1 FROM action_log b
                  WHERE b.plan_id = a.plan_id AND a.plan_id <> 0 AND b.id < a.id AND
                        b.event = 'transition' AND b.to_state = 'approved' AND
                        b.outcome = 'accepted' AND b.actor = 'owner')
ORDER BY a.id)sql");
    if (!query) {
      return tl::unexpected(query.error());
    }
    while (true) {
      Result<bool> row = query->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      const std::string to = query->column_text(3);
      found.push_back("plan " + std::to_string(query->column_int(1)) + ": " +
                      query->column_text(2) + " " + (to.empty() ? query->column_text(4) : to) +
                      " by " + query->column_text(5) + " (row " +
                      std::to_string(query->column_int(0)) + ")");
    }
  });
  if (!read) {
    found.push_back("action_log could not be read: " + read.error().message);
  }
  return found;
}

// Fails the test once per action of unapproved_actions(), naming it.
inline void expect_every_action_approved(store::Store& store) {
  for (const std::string& action : unapproved_actions(store)) {
    ADD_FAILURE() << "action without a tray approval: " << action;
  }
}

// ---- Fakes the unit tests do not have as shared headers ------------------------------------

// The Zone.Identifier streams of the Downloads files, by path.
class FakeZones final : public space::IZoneIdentifierReader {
 public:
  std::map<std::filesystem::path, std::string> streams;

  [[nodiscard]] Result<std::optional<std::string>> read(
      const std::filesystem::path& file) const override {
    const auto found = streams.find(file);
    return found == streams.end() ? std::nullopt : std::optional{found->second};
  }
};

// Installer files and the installed programs.
class FakeInstallers final : public space::IInstallerInspector {
 public:
  std::map<std::filesystem::path, std::string> product_codes;
  std::map<std::string, std::string> installed;  // product code -> name
  std::map<std::filesystem::path, space::VersionInfo> versions;
  std::vector<space::InstalledProgram> programs;

  [[nodiscard]] Result<std::optional<std::string>> msi_product_code(
      const std::filesystem::path& package) const override {
    const auto code = product_codes.find(package);
    return code == product_codes.end() ? std::nullopt : std::optional{code->second};
  }
  [[nodiscard]] Result<std::optional<std::string>> installed_msi_product(
      const std::string& product_code) const override {
    const auto product = installed.find(product_code);
    return product == installed.end() ? std::nullopt : std::optional{product->second};
  }
  [[nodiscard]] Result<std::optional<space::VersionInfo>> exe_version(
      const std::filesystem::path& executable) const override {
    const auto version = versions.find(executable);
    return version == versions.end() ? std::nullopt : std::optional{version->second};
  }
  [[nodiscard]] Result<std::vector<space::InstalledProgram>> installed_programs() const override {
    return programs;
  }
};

// The owner's Appx packages and the runs each usage source recorded.
class FakeAppUsage final : public apps::IAppSystem {
 public:
  std::vector<apps::AppxPackage> packages;
  std::vector<apps::RunRecord> user_assist_runs;
  std::vector<apps::RunRecord> prefetch_runs;
  std::vector<apps::RunRecord> bam_runs;
  std::string maker = "ASUS";

  [[nodiscard]] Result<std::vector<apps::AppxPackage>> appx() const override { return packages; }
  [[nodiscard]] Result<std::vector<apps::RunRecord>> user_assist() const override {
    return user_assist_runs;
  }
  [[nodiscard]] Result<std::vector<apps::RunRecord>> prefetch() const override {
    return prefetch_runs;
  }
  [[nodiscard]] Result<std::vector<apps::RunRecord>> bam() const override { return bam_runs; }
  [[nodiscard]] Result<std::string> manufacturer() const override { return maker; }
};

// The security component's view of the machine's startup items: the same fake the cleaning
// service reads and changes, so both see one machine.
class SharedStartup final : public startup::IStartupSystem {
 public:
  explicit SharedStartup(startup::IStartupSystem& inner) : inner_(&inner) {}

  [[nodiscard]] Result<startup::Snapshot> read() const override { return inner_->read(); }
  [[nodiscard]] Result<void> set_approved(startup::Scope scope, startup::ApprovedKey key,
                                          std::string_view name,
                                          const startup::ApprovedValue& value) override {
    return inner_->set_approved(scope, key, name, value);
  }
  [[nodiscard]] Result<void> set_task_enabled(std::string_view path, bool enabled) override {
    return inner_->set_task_enabled(path, enabled);
  }
  [[nodiscard]] Result<void> set_service_start(std::string_view name,
                                               const startup::ServiceStart& start) override {
    return inner_->set_service_start(name, start);
  }

 private:
  startup::IStartupSystem* inner_;
};

// Winlogon and AppInit as Windows ships them.
class FakePersistence final : public security::IPersistenceSystem {
 public:
  [[nodiscard]] security::PersistenceSnapshot read() const override {
    return security::PersistenceSnapshot{
        .winlogon = {{.name = "Shell", .value = "explorer.exe"},
                     {.name = "Userinit", .value = R"(C:\Windows\system32\userinit.exe,)"}},
        .appinit = {{.dlls = ""}},
        .system_folder = R"(C:\Windows\system32)"};
  }
};

// Edge: a store extension and one forced by policy; Chrome: one a program put there.
class FakeBrowsers final : public security::IBrowserProfiles {
 public:
  [[nodiscard]] security::BrowserSnapshot read() const override {
    return security::BrowserSnapshot{
        .chromium = {{.browser = security::Browser::edge,
                      .profile = "Default",
                      .secure_preferences = R"({"extensions": {"settings": {
                        "storestorestorestorestorestorest": {"location": 1, "from_webstore": true,
                          "manifest": {"name": "Password Manager", "version": "4.2"}},
                        "corpcorpcorpcorpcorpcorpcorpcorp": {"location": 1, "manifest":
                          {"name": "Corp Search", "version": "1"}}}}})"},
                     {.browser = security::Browser::chrome,
                      .profile = "Default",
                      .secure_preferences = R"({"extensions": {"settings": {
                        "toolbartoolbartoolbartoolbartool": {"location": 3, "manifest":
                          {"name": "Shopping Toolbar", "version": "9.1"}}}}})"}},
        .forced = {{security::Browser::edge, "corpcorpcorpcorpcorpcorpcorpcorp"}}};
  }
};

// The startup items' files exist on disk, nothing else.
class FakePaths final : public security::IPathResolver {
 public:
  [[nodiscard]] std::optional<std::filesystem::path> resolve(
      std::string_view target) const override {
    const std::filesystem::path path{target};
    for (const std::filesystem::path& known :
         {discord, steam, updater, agent, spotify, onedrive, armoury, teams, backup_agent}) {
      if (path == known) {
        return known;
      }
    }
    return std::nullopt;
  }
};

// Defender's history holds a detection of agent.exe. Counts file scans: none may run.
class FakeDefender final : public security::IDefender {
 public:
  [[nodiscard]] Result<std::vector<security::Detection>> detections() const override {
    return std::vector<security::Detection>{{.threat = "Trojan:Win32/Agent",
                                             .resources = {"file:_" + agent.string()},
                                             .time = now - 24h}};
  }
  [[nodiscard]] Result<security::FileScan> scan_file(const std::filesystem::path& /*file*/,
                                                     const std::stop_token& /*stop*/) override {
    ++*scans;
    return security::FileScan{};
  }

  int* scans = nullptr;
};

// Counts lookups: VirusTotal is off on this machine, so none may happen.
class FakeVirusTotal final : public security::IHashLookup {
 public:
  [[nodiscard]] Result<std::optional<security::HashReport>> lookup(
      std::string_view /*sha256*/, std::string_view /*api_key*/) override {
    ++*lookups;
    return std::optional<security::HashReport>{};
  }

  int* lookups = nullptr;
};

class FakeResolver final : public security::IResolver {
 public:
  [[nodiscard]] std::optional<std::string> name(std::string_view /*address*/) override {
    return std::nullopt;
  }
};

class FakeStamps final : public trust::IImageInspector {
 public:
  [[nodiscard]] Result<trust::FileStamp> stamp(
      const std::filesystem::path& /*image*/) const override {
    return trust::FileStamp{.size = 100, .modified = now - 48h};
  }
  [[nodiscard]] Result<trust::Signature> verify(
      const std::filesystem::path& /*image*/) const override {
    return trust::Signature{};
  }
};

// The backup state of the owner, which a test may change before the next check.
struct BackupState {
  backup::FileHistoryFacts history;
  std::vector<backup::RestorePoint> points;
};

class FakeBackupSystem final : public backup::IBackupSystem {
 public:
  explicit FakeBackupSystem(const BackupState& state) : state_(&state) {}

  [[nodiscard]] Result<backup::FileHistoryFacts> file_history() const override {
    return state_->history;
  }
  [[nodiscard]] Result<bool> image_versions() const override { return false; }
  [[nodiscard]] Result<std::vector<backup::RestorePoint>> restore_points() const override {
    return state_->points;
  }
  [[nodiscard]] Result<std::vector<backup::ShadowCopy>> shadow_copies() const override {
    return std::vector<backup::ShadowCopy>{};
  }
  [[nodiscard]] Result<std::vector<backup::OneDriveAccount>> onedrive() const override {
    return std::vector<backup::OneDriveAccount>{};
  }
  [[nodiscard]] Result<std::vector<std::string>> installed_programs() const override {
    return std::vector<std::string>{"Steam", "Discord"};
  }
  [[nodiscard]] Result<std::vector<backup::Volume>> volumes() const override {
    return std::vector<backup::Volume>{{.root = L"C:\\", .label = "SO"},
                                       {.root = L"K:\\", .label = "Backup 1"},
                                       {.root = L"L:\\", .label = "Backup 2"}};
  }

 private:
  const BackupState* state_;
};

class FakeRepoGit final : public backup::IRepoGit {
 public:
  std::map<std::filesystem::path, backup::RepoState> states;

  [[nodiscard]] Result<backup::RepoState> state(
      const std::filesystem::path& work_tree) const override {
    const auto found = states.find(work_tree);
    if (found == states.end()) {
      return fail(Errc::not_found, "not a repository");
    }
    return found->second;
  }
};

// A forwarding directory reader, for a component that owns its reader.
class SharedReader final : public space::IDirectoryReader {
 public:
  explicit SharedReader(const space::IDirectoryReader& inner) : inner_(&inner) {}
  [[nodiscard]] Result<std::vector<space::ListedEntry>> list(
      const std::filesystem::path& path) const override {
    return inner_->list(path);
  }

 private:
  const space::IDirectoryReader* inner_;
};

// The knowledge pack of the cleaning service: no clean rule (so nothing runs on its own), the
// knowledge-base entry of Old Tool.
inline constexpr std::string_view acceptance_manifest = R"toml(id = "acceptance"
version = "1.0.0"
title = "Acceptance"
description = "The acceptance suite's knowledge base."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)toml";

inline constexpr std::string_view acceptance_kb = R"json({"entries": [
  {"id": "old-tool", "image": "tool.exe",
   "description": "Old Tool, a file converter that nothing else needs.",
   "category": "utility", "needed_at_startup": false, "safe_to_remove": true}]})json";

// ---- The fixture ----------------------------------------------------------------------------

class AcceptanceTest : public ::testing::Test {
 protected:
  AcceptanceTest() {
    build_processes();
    build_startup();
    build_files();
    build_apps();
    build_network();
    build_backups();
  }

  void SetUp() override {
    open_store();
    start_trust();
    start_telemetry();
    start_security();
    start_cleaning();
    start_backup();
    start_report();
    initialize_mcp();
  }

  void TearDown() override {
    if (report_) {
      report_->stop();
    }
    if (backup_) {
      backup_->stop();
    }
    if (security_) {
      security_->stop();
    }
    cleaning_.reset();
    if (trust_) {
      trust_->stop();
    }
  }

  // ---- The MCP client side: what Claude Code sends --------------------------------------------

  // The raw response to one tools/call.
  Json call_tool(std::string_view name, const Json& arguments = Json::object()) {
    const Json request{{"jsonrpc", "2.0"},
                       {"id", ++request_id_},
                       {"method", "tools/call"},
                       {"params", {{"name", name}, {"arguments", arguments}}}};
    const std::optional<std::string> response = server_->handle(request.dump());
    EXPECT_TRUE(response.has_value()) << name;
    return response ? Json::parse(*response) : Json();
  }

  // The answer of tool `name`, parsed from its text; fails the test on a tool error.
  Json tool(std::string_view name, const Json& arguments = Json::object()) {
    const Json response = call_tool(name, arguments);
    const Json& result = response.at("result");
    const std::string text = result.at("content")[0].at("text").get<std::string>();
    EXPECT_FALSE(result.at("isError").get<bool>()) << name << ": " << text;
    return result.at("isError").get<bool>() ? Json() : Json::parse(text);
  }

  // The answer of a space or backup tool once its background analysis ran: the first call starts
  // it ("scanning" / "checking"), the service's worker runs it, the second call reads it.
  Json analysed(std::string_view name, const Json& arguments = Json::object()) {
    Json answer = tool(name, arguments);
    if (answer.value("state", "") == "scanning") {
      cleaning_->work_space({});
      answer = tool(name, arguments);
    } else if (name == "check_backups" && !answer.contains("sources")) {
      run_backup_check();
      answer = tool(name, arguments);
    }
    EXPECT_EQ(answer.value("state", ""), "ready") << name << ": " << answer.dump();
    return answer;
  }

  // The tools/list answer.
  Json tools() {
    const std::optional<std::string> response = server_->handle(
        Json{{"jsonrpc", "2.0"}, {"id", ++request_id_}, {"method", "tools/list"}}.dump());
    return response ? Json::parse(*response).at("result").at("tools") : Json();
  }

  // ---- The tray side: the owner's clicks ------------------------------------------------------

  Result<Json> tray_call(std::string_view method, const Json& params) {
    return ipc::parse_response(
        methods_.handle(ipc::make_request(1, method, params), ipc::Caller{.tray = true}), 1);
  }
  Json tray(std::string_view method, const Json& params) {
    Result<Json> result = tray_call(method, params);
    EXPECT_TRUE(result.has_value()) << method << ": " << result.error().message;
    return result.value_or(Json());
  }
  // A call any process but the tray makes, such as an MCP client trying plans.approve.
  Result<Json> other_call(std::string_view method, const Json& params) {
    return ipc::parse_response(methods_.handle(ipc::make_request(1, method, params)), 1);
  }

  // One pass of the cleaning worker: runs what the owner approved.
  void work() { cleaning_->work({}); }
  // The backup component's daily check, now.
  void run_backup_check() { backup_->check_now(std::stop_token{}); }
  // The monthly report's scheduler polled now, as its thread does every 30 s.
  void poll_reports() { report_->poll_now(std::stop_token{}); }
  // The report of the current month, made now (the scheduler's job).
  [[nodiscard]] Result<void> produce_report() { return report_->produce(std::stop_token{}); }
  // The connections, listeners and UDP endpoints the next network read sees.
  void set_connections(std::vector<events::Connection> connections) {
    connections_ = std::move(connections);
    connection_table_->set(connections_);
  }
  [[nodiscard]] const std::vector<events::Connection>& connections() const { return connections_; }
  // Where the monthly reports are written.
  [[nodiscard]] std::filesystem::path reports_folder() const { return folder_.path() / "reports"; }

  // ---- State -----------------------------------------------------------------------------------

  [[nodiscard]] store::Store& store() { return *store_; }
  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] fakes::FakeFileSystem& fs() { return fs_; }
  [[nodiscard]] startup::testing::FakeStartupSystem& startup_system() { return startup_; }
  [[nodiscard]] svc::testing::FakeMachine& machine() { return machine_; }
  [[nodiscard]] BackupState& backup_state() { return backup_state_; }
  [[nodiscard]] int defender_scans() const { return defender_scans_; }
  [[nodiscard]] int virustotal_lookups() const { return virustotal_lookups_; }

  // The telemetry pipeline samples after `elapsed`, PowerToys busy meanwhile.
  void sample(std::chrono::seconds elapsed) {
    svc::testing::burn_browser_cpu(*machine_.processes, elapsed);
    clock_.advance(elapsed);
    for (const Error& error : pipeline_->poll()) {
      ADD_FAILURE() << "telemetry: " << error.message;
    }
  }

  // Adds a file to the fake file system and to the directory listings of the space analyses,
  // with its parent folders.
  void add_file(const std::filesystem::path& path, std::uint64_t bytes, WallTime created,
                WallTime modified) {
    fs_.add_file(path, Bytes{bytes});
    space::ListedEntry entry = space::testing::file(path.filename().string(), bytes);
    ++file_number_;
    entry.identity.file_id[0] = static_cast<std::uint8_t>(file_number_ & 0xFFU);
    entry.identity.file_id[1] = static_cast<std::uint8_t>(file_number_ >> 8U);
    entry.created = created;
    entry.modified = modified;
    reader_.listings[path.parent_path()].push_back(std::move(entry));
    add_folder(path.parent_path());
  }

 private:
  // Lists `folder` in its parent, and so on up to the drive root.
  void add_folder(std::filesystem::path folder) {
    for (; folder != folder.root_path(); folder = folder.parent_path()) {
      std::vector<space::ListedEntry>& siblings = reader_.listings[folder.parent_path()];
      const std::string name = folder.filename().string();
      if (std::ranges::none_of(
              siblings, [&](const space::ListedEntry& entry) { return entry.name == name; })) {
        siblings.push_back(space::testing::folder(name));
      }
    }
  }

  void build_processes() {
    // As phase1_components(): pid 100 is PowerToys, signed by Microsoft in Program Files; pid 200
    // editor.exe, unsigned in the owner's AppData; pid 300 cannot be opened.
    machine_.processes->processes()[1].image = "PowerToys.exe";
    machine_.processes->processes()[2].image = "PowerToys.exe";
    const trust::Signature none{.status = trust::SignatureStatus::none, .signer = {}};
    const auto signed_by = [](std::string signer) {
      return trust::Signature{.status = trust::SignatureStatus::valid, .signer = std::move(signer)};
    };
    images_.paths = {{100, svc::testing::powertoys_path},
                     {101, svc::testing::powertoys_path},
                     {200, svc::testing::editor_path}};
    inspector_.signatures = {{svc::testing::powertoys_path, signed_by("Microsoft Corporation")},
                             {svc::testing::editor_path, none},
                             {discord.native(), signed_by("Discord Inc.")},
                             {steam.native(), signed_by("Valve Corp.")},
                             {updater.native(), none},
                             {agent.native(), none},
                             {spotify.native(), signed_by("Spotify AB")},
                             {onedrive.native(), signed_by("Microsoft Corporation")},
                             {armoury.native(), signed_by("ASUSTeK COMPUTER INC.")},
                             {teams.native(), signed_by("Microsoft Corporation")},
                             {backup_agent.native(), signed_by("Backup Co")}};
  }

  void build_startup() {
    startup_.snapshot = startup::Snapshot{
        .run = {{.name = "Discord", .command = quoted(discord) + " --processStart Discord.exe"},
                {.name = "Steam", .command = quoted(steam) + " -silent"},
                {.name = "Updater", .command = quoted(updater) + " /silent"},
                {.name = "Agent", .command = agent.string()}},
        .files = {{.file_name = "Spotify.lnk", .target = spotify.string()}},
        .tasks = {{.path = R"(\OneDrive Startup Task)",
                   .logon = true,
                   .command = quoted(onedrive) + " /background",
                   .signer = "Microsoft Corporation"}},
        .services = {{.name = "ArmouryCrateService",
                      .display_name = "ARMOURY CRATE Service",
                      .start = {.type = startup::StartType::automatic},
                      .binary = armoury.string(),
                      .signer = "ASUSTeK COMPUTER INC."}}};
  }

  void build_files() {
    const WallTime old = now - std::chrono::days{300};
    const WallTime recent = now - std::chrono::days{10};
    for (int index = 1; index <= 25; ++index) {
      add_file(std::filesystem::path(L"C:\\Data") / ("archive" + std::to_string(index) + ".bin"),
               static_cast<std::uint64_t>(index) << 30U, old, old);
    }
    // The owner's Downloads: an old download, two installers of installed programs, a new file.
    add_file(downloads / "setup-old.zip", 700ULL << 20U, old, old);
    zones_.streams[downloads / "setup-old.zip"] =
        "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://example.com/setup-old.zip\r\n";
    add_file(downloads / "OldTool-1.2.msi", 40ULL << 20U, recent, recent);
    installers_.product_codes[downloads / "OldTool-1.2.msi"] =
        "{11111111-2222-3333-4444-555555555555}";
    installers_.installed["{11111111-2222-3333-4444-555555555555}"] = "Old Tool";
    add_file(downloads / "SteamSetup.exe", 2ULL << 20U, recent, recent);
    installers_.versions[downloads / "SteamSetup.exe"] =
        space::VersionInfo{.product_name = "Steam", .company_name = "Valve Corporation"};
    installers_.programs = {{.display_name = "Steam", .publisher = "Valve Corporation"}};
    add_file(downloads / "report.pdf", 3ULL << 20U, recent, recent);
    // Pictures: beach7_small.jpg is a resized copy of beach7.jpg (the decoder sees one picture);
    // copy-of-1001.jpg has the content of IMG_1001.jpg (the hasher hashes by the digits).
    add_file(pictures / "beach7.jpg", 3ULL << 20U, recent, recent);
    add_file(pictures / "beach7_small.jpg", 1ULL << 20U, recent, recent);
    add_file(pictures / "IMG_1001.jpg", 5ULL << 20U, old, old);
    add_file(pictures / "copy-of-1001.jpg", 5ULL << 20U, recent, recent);
  }

  void build_apps() {
    const std::string root = R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";
    const auto add = [&](const std::string& subkey, const std::string& name,
                         const std::string& publisher, const std::string& location,
                         const std::string& icon, const std::string& uninstall) {
      const std::string key = root + "\\" + subkey;
      registry_.put(key, "DisplayName", placement::RegistryValue{.text = name});
      registry_.put(key, "Publisher", placement::RegistryValue{.text = publisher});
      registry_.put(key, "InstallLocation", placement::RegistryValue{.text = location});
      if (!icon.empty()) {
        registry_.put(key, "DisplayIcon", placement::RegistryValue{.text = icon});
      }
      registry_.put(key, "UninstallString", placement::RegistryValue{.text = uninstall});
    };
    add("OldTool", "Old Tool", "Tools Ltd", R"(C:\Program Files\Old Tool)",
        R"(C:\Program Files\Old Tool\tool.exe)", R"("C:\Program Files\Old Tool\unins000.exe")");
    add("BoardUtil", "Board Utility", "ASUSTeK COMPUTER INC.", R"(C:\Program Files\ASUS\Util)", "",
        R"(C:\Program Files\ASUS\Util\uninstall.exe)");
    add("VC", "Microsoft Visual C++ 2015-2022 Redistributable (x64)", "Microsoft Corporation",
        R"(C:\ProgramData\Package Cache\{X})", "",
        R"("C:\ProgramData\Package Cache\{X}\vc.exe" /u)");
    add("Launcher", "Game Launcher", "Some Games Inc", R"(C:\Program Files\Launcher)", "",
        R"(C:\Program Files\Launcher\uninstall.exe)");
    app_usage_.prefetch_runs = {{.source = apps::UsageSource::prefetch,
                                 .path = R"(C:\PROGRAM FILES\OLD TOOL\TOOL.EXE)",
                                 .last_run = now - std::chrono::days{200}}};
    app_usage_.user_assist_runs = {{.source = apps::UsageSource::user_assist,
                                    .path = R"(C:\Program Files\ASUS\Util\util.exe)",
                                    .last_run = now - std::chrono::days{400}}};
    app_usage_.bam_runs = {{.source = apps::UsageSource::bam,
                            .path = R"(C:\Program Files\Launcher\launcher.exe)",
                            .last_run = now - std::chrono::days{5}}};
    packs_.put(cleaning::testing::data_dir / "packs" / "acceptance" / "pack.toml",
               std::string(acceptance_manifest));
    packs_.put(cleaning::testing::data_dir / "packs" / "acceptance" / "kb.json",
               std::string(acceptance_kb));
  }

  void build_network() {
    const auto ipv4 = [](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
      events::IpAddress address;
      address.bytes = {a, b, c, d};
      return address;
    };
    connections_.push_back({.remote_address = ipv4(203, 0, 113, 9),
                            .remote_port = 443,
                            .state = events::TcpState::established,
                            .pid = 100});
    connections_.push_back({.remote_address = ipv4(198, 51, 100, 7),
                            .remote_port = 8443,
                            .state = events::TcpState::established,
                            .pid = 200});
    connections_.push_back({.local_port = 8080, .state = events::TcpState::listen, .pid = 200});
  }

  void build_backups() {
    backup_state_.points = {{.created = now - 24h, .description = "Windows Update"}};
    reader_.listings[L"K:\\"] = {};
    reader_.listings[L"L:\\"] = {};
    drives_.list() = placement::testing::owner_volumes();
    workspace_files_.put(cleaning::testing::data_dir / "workspaces.toml",
                         "roots = ['F:\\']\nskip = ['.git', 'node_modules']\n");
    for (const char* name : {"app", "lib", "ok"}) {
      reader_.listings[L"F:\\"].push_back(space::testing::folder(name));
      reader_.listings[std::filesystem::path(L"F:\\") / name] = {space::testing::folder(".git")};
    }
    repo_git_states_ = {{LR"(F:\app)", {.remotes = 0}},
                        {LR"(F:\lib)", {.remotes = 1, .unpushed = 3, .uncommitted = true}},
                        {LR"(F:\ok)", {.remotes = 1}}};
  }

  void open_store() {
    const std::vector<store::MigrationSet> sets = {
        actions::action_migrations(), cleaning::cleaning_migrations(),
        startup::impact_migrations(), resident::resident_migrations(),
        input::input_migrations(),    elimination::elimination_migrations(),
        events::event_migrations(),   security::security_migrations(),
        report::report_migrations()};
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(folder_.path() / "myclean.db", clock_, sets);
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    // One logon MyClean measured (the startup component's record), naming every startup program.
    const std::vector<startup::ImageImpact> logon = {
        {.image = "update.exe", .cpu_seconds = 4.0, .io_bytes = 90ULL << 20U},
        {.image = "steam.exe", .cpu_seconds = 6.5, .io_bytes = 300ULL << 20U},
        {.image = "upd.exe", .cpu_seconds = 0.5, .io_bytes = 1ULL << 20U},
        {.image = "agent.exe", .cpu_seconds = 1.0, .io_bytes = 2ULL << 20U},
        {.image = "spotify.exe", .cpu_seconds = 2.0, .io_bytes = 40ULL << 20U},
        {.image = "onedrive.exe", .cpu_seconds = 3.0, .io_bytes = 120ULL << 20U},
        {.image = "armourycrate.service.exe", .cpu_seconds = 5.0, .io_bytes = 60ULL << 20U}};
    ASSERT_TRUE(startup::record_logon(*store_, now - 3h, logon).has_value());
    // editor.exe sent 20 kB to 198.51.100.7:8443 once an hour for the last six hours, with no
    // window: a background talker in the events component's network history.
    events::EventRecorder recorder(*store_);
    const auto start = std::make_shared<const events::ProcessStart>(events::ProcessStart{
        .process = {.pid = 200, .created = svc::testing::fake_boot + std::chrono::seconds{200}},
        .image = R"(C:\Users\owner\AppData\Local\Editor\editor.exe)"});
    for (int hour = 6; hour >= 1; --hour) {
      recorder.on_network(events::NetworkTransfer{.time = now - std::chrono::hours{hour},
                                                  .pid = 200,
                                                  .remote_address = editor_remote(),
                                                  .remote_port = 8443,
                                                  .bytes = 20'000,
                                                  .process = start});
    }
    recorder.close_all();
    ASSERT_TRUE(store_->flush().has_value());
  }

  static events::IpAddress editor_remote() {
    events::IpAddress address;
    address.bytes = {198, 51, 100, 7};
    return address;
  }

  [[nodiscard]] svc::Context context(std::filesystem::path data_dir) {
    return {*store_, methods_, clock_, std::move(data_dir), std::string(owner_sid)};
  }

  void start_trust() {
    auto inspector = std::make_unique<ForwardingInspector>(inspector_);
    auto images = std::make_unique<ForwardingImages>(images_);
    trust_ = std::make_unique<svc::TrustComponent>(
        svc::TrustSources{
            .inspector = std::move(inspector),
            .images = std::move(images),
            .user_writable_roots = std::vector<std::filesystem::path>{LR"(C:\Users\owner)"}},
        shipped_packs_);
    ASSERT_TRUE(trust_->start(context(svc::testing::shipped_data)).has_value());
  }

  void start_telemetry() {
    pipeline_ = std::make_unique<telemetry::Pipeline>(clock_, *store_);
    collectors_ = svc::add_collectors(*pipeline_, machine_.sources);
    telemetry_ = std::make_unique<svc::TelemetryMethods>(
        collectors_, *store_, clock_, machine_.sources,
        [this](std::uint32_t pid, WallTime created) { return trust_->process(pid, created); });
    ASSERT_TRUE(telemetry_->register_methods(methods_).has_value());
    for (const Error& error : pipeline_->poll()) {
      ADD_FAILURE() << "telemetry: " << error.message;
    }
  }

  void start_security() {
    auto defender = std::make_unique<FakeDefender>();
    defender->scans = &defender_scans_;
    auto virustotal = std::make_unique<FakeVirusTotal>();
    virustotal->lookups = &virustotal_lookups_;
    auto connections = std::make_unique<events::testing::FakeConnectionTable>();
    connections->set(connections_);
    connection_table_ = connections.get();
    security_ = std::make_unique<svc::SecurityComponent>(svc::SecuritySources{
        .startup = [this](std::string_view) -> std::unique_ptr<startup::IStartupSystem> {
          return std::make_unique<SharedStartup>(startup_);
        },
        .persistence = [](std::string_view) -> std::unique_ptr<security::IPersistenceSystem> {
          return std::make_unique<FakePersistence>();
        },
        .browsers = [](std::string_view) -> std::unique_ptr<security::IBrowserProfiles> {
          return std::make_unique<FakeBrowsers>();
        },
        .paths = [](std::string_view) -> std::unique_ptr<security::IPathResolver> {
          return std::make_unique<FakePaths>();
        },
        .defender = std::move(defender),
        .stamps = std::make_unique<FakeStamps>(),
        .hasher = std::make_unique<cleaning::testing::NameDigitHasher>(),
        .virustotal = std::move(virustotal),
        .resolver = std::make_unique<FakeResolver>(),
        .connections = std::move(connections),
        .settings = []() -> Result<std::optional<std::string>> { return std::nullopt; },
        .image_trust = [this](const std::filesystem::path& image) { return trust_->image(image); },
        .process_trust = [this](std::uint32_t pid,
                                WallTime created) { return trust_->process(pid, created); },
        .view = [this] { return collectors_.live.processes->latest(); },
        .period = std::chrono::milliseconds::zero()});
    ASSERT_TRUE(security_->start(context(svc::testing::shipped_data)).has_value());
  }

  void start_cleaning() {
    cleaning::ServiceSources sources{
        .store = store_.get(),
        .clock = &clock_,
        .file_system = &fs_,
        .probes = {.tree = &tree_,
                   .usage = &usage_,
                   .drives = &drives_,
                   .startup = &startup_,
                   .alerts = &alerts_},
        .owner = [this]() -> Result<cleaning::OwnerContext> {
          return cleaning::OwnerContext{
              .environment = std::make_unique<clean::testing::FakeEnvironment>(),
              .locations = actions::testing::owner_locations(),
              .downloads = downloads,
              .tools = std::shared_ptr<const placement::Tools>(&owner_tools_,
                                                               [](const placement::Tools*) {})};
        },
        .files = &packs_,
        .data_dir = cleaning::testing::data_dir,
        .space = {.reader = &reader_,
                  .zones = &zones_,
                  .hasher = &hasher_,
                  .installers = &installers_,
                  .images = &decoder_},
        .apps = &app_usage_};
    cleaning_ = std::make_unique<cleaning::CleaningService>(std::move(sources),
                                                            cleaning::ServiceSettings{});
    ASSERT_TRUE(cleaning_->start(methods_).has_value());
    work();  // the owner's session and folders are known from the first pass on
  }

  void start_backup() {
    backup_ = std::make_unique<svc::BackupComponent>(
        svc::BackupSources{
            .system = [this](std::string_view) -> std::unique_ptr<backup::IBackupSystem> {
              return std::make_unique<FakeBackupSystem>(backup_state_);
            },
            .reader = std::make_unique<SharedReader>(reader_),
            .drives = std::make_unique<SharedDrives>(drives_),
            .git = [this](std::string_view) -> std::unique_ptr<backup::IRepoGit> {
              auto git = std::make_unique<FakeRepoGit>();
              git->states = repo_git_states_;
              return git;
            },
            .files = &workspace_files_,
            .period = {}},
        &alerts_);
    ASSERT_TRUE(backup_->start(context(cleaning::testing::data_dir)).has_value());
  }

  // Last, as in myclean-svc: the monthly report calls the other components' read methods. Its
  // calls stand in for the service's space worker too: a storage analysis it starts runs at once.
  void start_report() {
    report_ = std::make_unique<svc::ReportComponent>(
        svc::ReportSources{
            .call = [this](std::string_view method, const Json& params) -> Result<Json> {
              Result<Json> answer = other_call(method, params);
              if (answer && answer->value("state", "") == "scanning") {
                cleaning_->work_space({});
                answer = other_call(method, params);
              }
              return answer;
            },
            .reports_dir = folder_.path() / "reports",
            .warm_up = std::chrono::seconds::zero(),
            .poll_period = std::chrono::seconds::zero(),
            .analysis_poll = std::chrono::seconds::zero()},
        &alerts_);
    ASSERT_TRUE(report_->start(context(cleaning::testing::data_dir)).has_value());
  }

  void initialize_mcp() {
    server_ = std::make_unique<mcp::Server>(
        [this](std::string_view method, const Json& params) -> mcp::Reply {
          // The MCP server reaches the service as any pipe client but the tray does.
          Result<Json> result = other_call(method, params);
          if (!result) {
            return tl::unexpected(
                mcp::CallError{.reached = true, .message = result.error().message});
          }
          return std::move(*result);
        },
        clock_, "acceptance");
    const std::optional<std::string> response = server_->handle(Json{
        {"jsonrpc", "2.0"},
        {"id", ++request_id_},
        {"method", "initialize"},
        {"params", {{"protocolVersion", "2025-06-18"}, {"clientInfo", {{"name", client_name}}}}}}
                                                                    .dump());
    ASSERT_TRUE(response.has_value());
  }

  class ForwardingInspector final : public trust::IImageInspector {
   public:
    explicit ForwardingInspector(const trust::IImageInspector& inner) : inner_(&inner) {}
    [[nodiscard]] Result<trust::FileStamp> stamp(
        const std::filesystem::path& image) const override {
      return inner_->stamp(image);
    }
    [[nodiscard]] Result<trust::Signature> verify(
        const std::filesystem::path& image) const override {
      return inner_->verify(image);
    }

   private:
    const trust::IImageInspector* inner_;
  };

  class ForwardingImages final : public trust::IProcessImages {
   public:
    explicit ForwardingImages(const trust::IProcessImages& inner) : inner_(&inner) {}
    [[nodiscard]] Result<std::filesystem::path> image_path(std::uint32_t pid,
                                                           WallTime created) const override {
      return inner_->image_path(pid, created);
    }

   private:
    const trust::IProcessImages* inner_;
  };

  class SharedDrives final : public placement::IDriveFacts {
   public:
    explicit SharedDrives(const placement::IDriveFacts& inner) : inner_(&inner) {}
    [[nodiscard]] Result<std::vector<placement::TargetVolume>> volumes() const override {
      return inner_->volumes();
    }

   private:
    const placement::IDriveFacts* inner_;
  };

  // The machine.
  actions::testing::TempFolder folder_;
  fakes::FakeClock clock_{now};
  svc::testing::FakeMachine machine_ = svc::testing::fake_machine();
  svc::testing::FakeImageInspector inspector_;
  svc::testing::FakeProcessImages images_;
  packs::DiskPackFiles shipped_packs_;
  fakes::FakeFileSystem fs_;
  cleaning::testing::FsTree tree_{fs_, now - std::chrono::days{30}};
  clean::testing::FakeUsage usage_;
  startup::testing::FakeStartupSystem startup_;
  placement::testing::FakeDriveFacts drives_;
  placement::testing::FakeRegistry registry_;
  placement::Tools owner_tools_{.registry = &registry_};
  space::testing::FakeDirectoryReader reader_;
  FakeZones zones_;
  FakeInstallers installers_;
  cleaning::testing::NameDigitHasher hasher_;
  cleaning::testing::NameDigitDecoder decoder_;
  FakeAppUsage app_usage_;
  packs::testing::FakePackFiles packs_;
  cleaning::testing::AlertRecorder alerts_;
  std::vector<events::Connection> connections_;
  BackupState backup_state_;
  rules::FakeDataFiles workspace_files_;
  std::map<std::filesystem::path, backup::RepoState> repo_git_states_;
  int defender_scans_ = 0;
  int virustotal_lookups_ = 0;
  std::uint32_t file_number_ = 0;

  // The service.
  std::unique_ptr<store::Store> store_;
  ipc::Dispatcher methods_;
  std::unique_ptr<svc::TrustComponent> trust_;
  std::unique_ptr<telemetry::Pipeline> pipeline_;
  svc::Collectors collectors_;
  std::unique_ptr<svc::TelemetryMethods> telemetry_;
  std::unique_ptr<svc::SecurityComponent> security_;
  std::unique_ptr<cleaning::CleaningService> cleaning_;
  std::unique_ptr<svc::BackupComponent> backup_;
  std::unique_ptr<svc::ReportComponent> report_;
  events::testing::FakeConnectionTable* connection_table_ = nullptr;  // owned by security_

  // The MCP client.
  std::unique_ptr<mcp::Server> server_;
  std::int64_t request_id_ = 0;
};

}  // namespace myclean::acceptance
