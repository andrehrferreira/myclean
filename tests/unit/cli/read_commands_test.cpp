#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/apps.hpp"
#include "core/ipc/backup.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
#include "core/ipc/reports.hpp"
#include "core/ipc/resident.hpp"
#include "core/ipc/security.hpp"
#include "core/model/result.hpp"
#include "fakes/fake_clock.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ipc/security.hpp"
#include "svc/svc_test_support.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;
using ipc::Param;
using ipc::ParamType;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 11 / 2026} + 12h;
constexpr std::int64_t noon_seconds = 1'794'398'400;  // noon as Unix seconds
constexpr std::int64_t day = 86'400;

const Param refresh{.name = "refresh", .type = ParamType::boolean};

struct Outcome {
  int code = -1;
  std::string out;
  std::string err;
};

// The read commands against a fake service: a pipe of its own answered by a dispatcher that
// declares each method's parameters as the service does (a parameter of the wrong type or an
// undeclared one is refused with -32602) and returns a canned answer. Each test sees the
// parameters the CLI sent and the text it printed.
class CliReadTest : public ::testing::Test {
 protected:
  // Registers `method` before the first command runs.
  void serve(std::string_view method, std::vector<Param> params, Json answer) {
    ASSERT_EQ(server_, nullptr) << "serve() after the first command";
    const std::string name(method);
    ASSERT_TRUE(
        methods_
            .add(name, std::move(params),
                 // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc
                 [this, name, answer = std::move(answer)](const Json& sent) -> Result<Json> {
                   const std::scoped_lock lock(mutex_);
                   sent_[name] = sent;
                   return answer;
                 })
            .has_value());
  }

  // Registers `method` as failing with `code` and `message`.
  void refuse(std::string_view method, std::vector<Param> params, Errc code, std::string message) {
    ASSERT_TRUE(methods_
                    .add(std::string(method), std::move(params),
                         [code, message = std::move(message)](const Json&) -> Result<Json> {
                           return fail(code, message);
                         })
                    .has_value());
  }

  [[nodiscard]] Outcome cli(std::vector<std::string> args) {
    if (server_ == nullptr) {
      const Result<std::string> owner = platform::current_user_sid();
      EXPECT_TRUE(owner.has_value());
      Result<std::unique_ptr<platform::PipeServer>> server =
          platform::PipeServer::start(platform::PipeServerOptions{
              .name = pipe_,
              .owner_sid = owner.value_or(""),
              .handler =
                  [this](std::string_view request) { return methods_.handle_frame(request); },
              .on_error = {}});
      EXPECT_TRUE(server.has_value()) << server.error().message;
      server_ = server ? std::move(*server) : nullptr;
    }
    std::string name;
    for (const wchar_t c : pipe_.substr(pipe_.rfind(L'\\') + 1)) {
      name.push_back(static_cast<char>(c));  // test pipe names are ASCII
    }
    args.insert(args.begin(), {"--pipe", name});
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    const int code = run(args, in, out, err, clock_, 5s);
    return Outcome{.code = code, .out = out.str(), .err = err.str()};
  }

  // The parameters the last call of `method` carried (null when it was not called).
  [[nodiscard]] Json sent(std::string_view method) {
    const std::scoped_lock lock(mutex_);
    const auto found = sent_.find(std::string(method));
    return found == sent_.end() ? Json() : found->second;
  }

  // Runs `command` as text and with --json: both exit 0, the JSON is the service's answer
  // unchanged, and the text is returned.
  std::string both_forms(std::vector<std::string> command, const Json& answer) {
    const Outcome human = cli(command);
    EXPECT_EQ(human.code, exit_ok) << command.at(0) << ": " << human.err;
    EXPECT_TRUE(human.err.empty()) << human.err;
    command.emplace_back("--json");
    const Outcome json = cli(command);
    EXPECT_EQ(json.code, exit_ok) << command.at(0) << ": " << json.err;
    EXPECT_TRUE(json.out.ends_with("\n"));
    EXPECT_EQ(Json::parse(json.out, nullptr, false), answer) << json.out;
    return human.out;
  }

 private:
  fakes::FakeClock clock_{noon};
  std::wstring pipe_ = svc::testing::unique_pipe_name();
  ipc::Dispatcher methods_;
  std::mutex mutex_;
  std::map<std::string, Json> sent_;
  std::unique_ptr<platform::PipeServer> server_;
};

// Expects `text` to hold `part`.
void expect_has(const std::string& text, std::string_view part) {
  EXPECT_NE(text.find(part), std::string::npos) << "missing \"" << part << "\" in:\n" << text;
}

Outcome usage_of(const std::vector<std::string>& args) {
  std::istringstream in;
  std::ostringstream out;
  std::ostringstream err;
  const fakes::FakeClock clock{noon};
  const int code = run(args, in, out, err, clock, 1s);
  return Outcome{.code = code, .out = out.str(), .err = err.str()};
}

// ---- space --------------------------------------------------------------------------------------

Json ready(Json answer) {
  answer["state"] = "ready";
  answer["scanned_at"] = noon_seconds;
  answer["error"] = nullptr;
  return answer;
}

TEST_F(CliReadTest, SpaceTopSendsTheDriveTokenAndListsFilesAndFolders) {
  const Json answer = ready({{"path", R"(C:\)"},
                             {"files", Json::array({Json{{"id", 7},
                                                         {"path", R"(C:\pagefile.sys)"},
                                                         {"size_on_disk", 17179869184},
                                                         {"size", 17179869184},
                                                         {"modified", noon_seconds},
                                                         {"protected", "page_file"}},
                                                    Json{{"id", 8},
                                                         {"path", R"(C:\Users\owner\big.iso)"},
                                                         {"size_on_disk", 5368709120},
                                                         {"size", 5368709120},
                                                         {"modified", noon_seconds}}})},
                             {"folders", Json::array({Json{{"path", R"(C:\Users)"},
                                                           {"size_on_disk", 107374182400},
                                                           {"size", 107374182400},
                                                           {"files", 1234}}})}});
  serve(ipc::space_top_method,
        {Param{.name = "path", .type = ParamType::token, .required = true},
         Param{.name = "n", .type = ParamType::integer}, refresh},
        answer);

  const std::string text = both_forms({"space", "top", "C:", "--limit", "5", "--refresh"}, answer);
  EXPECT_EQ(sent(ipc::space_top_method), (Json{{"path", "C"}, {"n", 5}, {"refresh", true}}));
  expect_has(text, R"(Largest files and folders of C:\, analysed 2026-11-11 12:00:00 UTC)");
  expect_has(text,
             R"([7]   16.0 GiB  C:\pagefile.sys  (protected: page_file; it cannot be proposed))");
  expect_has(text, "[8]    5.0 GiB  C:\\Users\\owner\\big.iso\n");
  expect_has(text, R"(  100.0 GiB  C:\Users (1234 files))");

  static_cast<void>(cli({"space", "top", "downloads"}));
  EXPECT_EQ(sent(ipc::space_top_method), (Json{{"path", "downloads"}, {"refresh", false}}));
}

TEST_F(CliReadTest, SpaceAnalysesShowTheirResults) {
  const Json downloads =
      ready({{"folder", R"(C:\Users\owner\Downloads)"},
             {"files", Json::array({Json{{"id", 1},
                                         {"path", R"(C:\Users\owner\Downloads\setup.zip)"},
                                         {"size", 1048576},
                                         {"size_on_disk", 1048576},
                                         {"created", 0},
                                         {"modified", 0},
                                         {"age_days", 200},
                                         {"origin",
                                          {{"zone_id", 3},
                                           {"host_url", "https://example.org/setup.zip"},
                                           {"referrer_url", nullptr}}}}})}});
  const Json forgotten = ready({{"roots", Json::array({R"(C:\Users\owner\Videos)"})},
                                {"min_size", 536870912},
                                {"idle_days", 180},
                                {"files", Json::array({Json{{"id", 2},
                                                            {"path", R"(C:\Users\owner\a.mkv)"},
                                                            {"size", 2147483648},
                                                            {"size_on_disk", 2147483648},
                                                            {"created", 0},
                                                            {"modified", 0},
                                                            {"idle_days", 400}}})}});
  const Json installers = ready(
      {{"folder", R"(C:\Users\owner\Downloads)"},
       {"used", Json::array({Json{{"id", 3},
                                  {"path", R"(C:\Users\owner\Downloads\tool.msi)"},
                                  {"kind", "msi"},
                                  {"size", 3145728},
                                  {"size_on_disk", 3145728},
                                  {"installed_as", "Tool 2.0"},
                                  {"product_code", "{1}"}}})},
       {"errors",
        Json::array({Json{{"path", R"(C:\Users\owner\Downloads\x.exe)"},
                          {"error", {{"code", "io_error"}, {"message", "sharing violation"}}}}})}});
  const Json decisions =
      ready({{"profile", R"(C:\Users\owner)"},
             {"cold", Json::array({Json{{"id", 4},
                                        {"path", R"(C:\Users\owner\Old)"},
                                        {"bytes", 3221225472},
                                        {"target", R"(E:\Archive\C\Users\owner\Old)"},
                                        {"reason", "nothing written since 2025-01-02"}}})},
             {"uninstall", Json::array({Json{{"id", 5},
                                             {"program", "Big Game"},
                                             {"publisher", "Studio"},
                                             {"version", "1.0"},
                                             {"location", R"(D:\Games\Big)"},
                                             {"bytes", 64424509440},
                                             {"area", "games/library"},
                                             {"reason", "not run for 300 days"}}})},
             {"notes", Json::array({"no HDD is attached"})}});
  const Json dev_drive =
      ready({{"supported", true},
             {"build", "26200.7171"},
             {"dev_drives", Json::array()},
             {"explanation", "A Dev Drive is a ReFS volume for source code."},
             {"repositories",
              Json::array({Json{
                  {"path", R"(F:\MyClean)"}, {"bytes", 2147483648}, {"changed", noon_seconds}}})},
             {"caches", Json::array({Json{{"area", "rust/cargo"},
                                          {"label", "Cargo cache"},
                                          {"path", R"(C:\Users\owner\.cargo)"},
                                          {"bytes", 1073741824},
                                          {"relocation", "cargo"},
                                          {"note", ""}}})},
             {"total_bytes", 3221225472},
             {"recommended_bytes", 53687091200},
             {"host", {{"volume", "D"}, {"free", 536870912000}, {"drive_class", "nvme"}}},
             {"steps", Json::array({"Open Settings > System > Storage.", "Create the Dev Drive."})},
             {"notes", Json::array()},
             {"sources", Json::array({"https://learn.microsoft.com/windows/dev-drive/"})}});
  const Json vhdx = ready({{"disks", Json::array({Json{{"id", 6},
                                                       {"path", R"(C:\wsl\ext4.vhdx)"},
                                                       {"area", "wsl/distro"},
                                                       {"label", "WSL distribution disk"},
                                                       {"distribution", "Ubuntu"},
                                                       {"size", 10737418240},
                                                       {"size_on_disk", 10737418240},
                                                       {"in_use", true}}})},
                           {"total", 10737418240},
                           {"notes", Json::array()}});
  const Json indexed = ready({{"folders", Json::array({Json{{"id", 9},
                                                            {"path", R"(F:\MyClean\build)"},
                                                            {"category", "build-artifacts"},
                                                            {"label", "CMake build tree"}}})}});
  for (const auto& [method, answer] : std::vector<std::pair<std::string_view, Json>>{
           {ipc::space_old_downloads_method, downloads},
           {ipc::space_forgotten_files_method, forgotten},
           {ipc::space_used_installers_method, installers},
           {ipc::space_owner_decisions_method, decisions},
           {ipc::space_dev_drive_method, dev_drive},
           {ipc::space_vhdx_method, vhdx},
           {ipc::space_indexed_folders_method, indexed}}) {
    serve(method, {refresh}, answer);
  }

  std::string text = both_forms({"space", "old-downloads"}, downloads);
  EXPECT_EQ(sent(ipc::space_old_downloads_method), (Json{{"refresh", false}}));
  expect_has(text,
             "Downloads created and modified more than 90 days ago in "
             R"(C:\Users\owner\Downloads, analysed 2026-11-11 12:00:00 UTC)");
  expect_has(text, R"([1]    1.0 MiB  C:\Users\owner\Downloads\setup.zip, 200 days old, from )"
                   "https://example.org/setup.zip");

  text = both_forms({"space", "forgotten-files", "--refresh"}, forgotten);
  EXPECT_EQ(sent(ipc::space_forgotten_files_method), (Json{{"refresh", true}}));
  expect_has(text, "Large forgotten files (at least 512.0 MiB on disk, untouched for 180 days)");
  expect_has(text, R"([2]    2.0 GiB  C:\Users\owner\a.mkv, untouched for 400 days)");

  text = both_forms({"space", "used-installers"}, installers);
  expect_has(text, R"([3]    3.0 MiB  C:\Users\owner\Downloads\tool.msi (msi, installed as )"
                   "Tool 2.0)");
  expect_has(text, R"(not read: C:\Users\owner\Downloads\x.exe (sharing violation))");

  text = both_forms({"space", "owner-decisions"}, decisions);
  expect_has(text,
             "[4]    3.0 GiB  C:\\Users\\owner\\Old\n      would be archived to "
             R"(E:\Archive\C\Users\owner\Old; nothing written since 2025-01-02)");
  expect_has(text, "[5]   60.0 GiB  Big Game 1.0 (Studio): not run for 300 days");
  expect_has(text, "Note: no HDD is attached");

  text = both_forms({"space", "dev-drive"}, dev_drive);
  expect_has(text, "Windows build 26200.7171: a Dev Drive is supported");
  expect_has(text, "Dev Drives here:\n  none\n");
  expect_has(text, "To move to a Dev Drive (3.0 GiB in all; recommended size 50.0 GiB)");
  expect_has(text, R"(    2.0 GiB  repository F:\MyClean, changed 2026-11-11 12:00:00 UTC)");
  expect_has(text, R"(    1.0 GiB  Cargo cache C:\Users\owner\.cargo (MyClean relocates it as )"
                   "cargo)");
  expect_has(text, "Where a new one would go: D (nvme, 500.0 GiB free)");
  expect_has(text, "  2. Create the Dev Drive.");

  text = both_forms({"space", "vhdx"}, vhdx);
  expect_has(text, "10.0 GiB on disk in all");
  expect_has(text, R"([6]   10.0 GiB  C:\wsl\ext4.vhdx  WSL distribution disk Ubuntu, in use)");

  text = both_forms({"space", "indexed-folders"}, indexed);
  expect_has(text, R"([9] F:\MyClean\build  [build-artifacts] CMake build tree)");
}

TEST_F(CliReadTest, SpaceDuplicatesSendsTheModeAndMarksThePhotoToKeep) {
  const Json photos =
      ready({{"mode", "photos"},
             {"roots", Json::array({R"(C:\Users\owner\Pictures)"})},
             {"groups", Json::array({Json{{"distance", 2},
                                          {"reclaimable", 2097152},
                                          {"files", Json::array({Json{{"id", 11},
                                                                      {"path", R"(C:\p\a.jpg)"},
                                                                      {"size_on_disk", 4194304},
                                                                      {"modified", 0},
                                                                      {"width", 4000},
                                                                      {"height", 3000},
                                                                      {"hash", "ff"},
                                                                      {"keep", true}},
                                                                 Json{{"id", 12},
                                                                      {"path", R"(C:\p\b.jpg)"},
                                                                      {"size_on_disk", 2097152},
                                                                      {"modified", 0},
                                                                      {"width", 2000},
                                                                      {"height", 1500},
                                                                      {"hash", "fe"},
                                                                      {"keep", false}}})}}})},
             {"reclaimable", 2097152},
             {"analysed", 2},
             {"not_analysed", Json::array()},
             {"groups_total", 1},
             {"truncated", true}});
  serve(ipc::space_duplicates_method, {refresh, Param{.name = "mode", .type = ParamType::token}},
        photos);

  const std::string text = both_forms({"space", "duplicates", "--mode", "photos"}, photos);
  EXPECT_EQ(sent(ipc::space_duplicates_method), (Json{{"refresh", false}, {"mode", "photos"}}));
  expect_has(text, "Near-duplicate photos, analysed");
  expect_has(text, "1 groups, 2.0 MiB reclaimable");
  expect_has(text, "  2.0 MiB reclaimable, photos differ in at most 2 bits:");
  expect_has(text, "    [11]    4.0 MiB  C:\\p\\a.jpg  4000x3000, the one to keep\n");
  expect_has(text, "    [12]    2.0 MiB  C:\\p\\b.jpg  2000x1500\n");
  expect_has(text, "more groups than one answer holds");

  static_cast<void>(cli({"space", "duplicates"}));
  EXPECT_EQ(sent(ipc::space_duplicates_method), (Json{{"refresh", false}}));
}

TEST_F(CliReadTest, ASpaceAnalysisThatIsStillRunningSaysSo) {
  serve(ipc::space_vhdx_method, {refresh},
        Json{{"state", "scanning"}, {"scanned_at", nullptr}, {"error", nullptr}});
  serve(ipc::space_indexed_folders_method, {refresh},
        Json{{"state", "failed"},
             {"scanned_at", noon_seconds},
             {"error", "Windows Search is not running"}});
  const Outcome running = cli({"space", "vhdx"});
  EXPECT_EQ(running.code, exit_ok);
  EXPECT_EQ(running.out,
            "WSL and Docker virtual disks\n  The analysis is running: run the "
            "command again in a moment.\n");
  expect_has(cli({"space", "indexed-folders"}).out,
             "  The analysis failed: Windows Search is not running\n");
}

TEST(CliReadUsageTest, SpaceUsageErrorsExitThree) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{{"space"},
                                             {"space", "top"},
                                             {"space", "top", "C", "D"},
                                             {"space", "frobnicate"},
                                             {"space", "vhdx", "extra"},
                                             {"space", "top", "C", "--limit", "five"}}) {
    const Outcome outcome = usage_of(args);
    EXPECT_EQ(outcome.code, exit_usage) << args.back();
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_FALSE(outcome.err.empty());
  }
}

// ---- startup, plans, quarantine -----------------------------------------------------------------

TEST_F(CliReadTest, StartupListsItemsWithTheirImpactAndBootTimes) {
  const Json items{
      {"items",
       Json::array(
           {Json{{"id", 1},
                 {"key", "run|user|Discord"},
                 {"source", "run"},
                 {"scope", "user"},
                 {"name", "Discord"},
                 {"display_name", "Discord"},
                 {"command", R"(C:\Users\owner\Discord\Update.exe --processStart Discord.exe)"},
                 {"enabled", true},
                 {"signer", "Discord Inc."},
                 {"impact",
                  {{"source", "myclean"},
                   {"cpu_seconds", 3.5},
                   {"io_bytes", 47185920},
                   {"logons", 5},
                   {"delay_seconds", 0}}}},
            Json{{"id", 2},
                 {"key", "service|machine|Spooler"},
                 {"source", "service"},
                 {"scope", "machine"},
                 {"name", "Spooler"},
                 {"display_name", "Print Spooler"},
                 {"command", R"(C:\Windows\System32\spoolsv.exe)"},
                 {"enabled", false},
                 {"start", "auto"},
                 {"delayed", true},
                 {"impact", nullptr}},
            Json{{"id", 3},
                 {"key", "runonce|user|Setup"},
                 {"source", "runonce"},
                 {"scope", "user"},
                 {"name", "Setup"},
                 {"display_name", "Setup"},
                 {"command", "setup.exe /finish"},
                 {"enabled", nullptr},
                 {"impact",
                  {{"source", "diagnostics"},
                   {"cpu_seconds", 0},
                   {"io_bytes", 0},
                   {"logons", 0},
                   {"delay_seconds", 4.2}}}}})}};
  const Json boots{
      {"source", "diagnostics"},
      {"log_error", nullptr},
      {"latest",
       {{"start", noon_seconds},
        {"boot_seconds", 35.2},
        {"main_path_seconds", 20.1},
        {"post_boot_seconds", 15.1},
        {"logon_seconds", 12.3},
        {"boot_difference_seconds", 3.0},
        {"logon_difference_seconds", -1.0},
        {"logon", 7},
        {"contributors",
         Json::array(
             {Json{{"image", "discord.exe"},
                   {"cpu_seconds", 3.5},
                   {"io_bytes", 47185920},
                   {"item", {{"name", "Discord"}, {"source", "run"}, {"scope", "user"}}}}})}}},
      {"median", {{"boot_seconds", 32.2}, {"logon_seconds", 13.3}, {"boots", 9}}},
      {"changes",
       Json::array({Json{
           {"plan", 4},
           {"applied", noon_seconds},
           {"items", Json::array({Json{{"name", "Teams"}, {"source", "run"}, {"scope", "user"}}})},
           {"before", {{"logon_seconds", 20.0}, {"boots", 5}}},
           {"after", nullptr}}})}};
  serve(ipc::startup_list_method, {}, items);
  serve(ipc::startup_boot_times_method, {}, boots);

  std::string text = both_forms({"startup", "list"}, items);
  EXPECT_EQ(sent(ipc::startup_list_method), Json::object());
  expect_has(text,
             "[1] on   Discord (run, user), signed by Discord Inc.\n"
             "      C:\\Users\\owner\\Discord\\Update.exe --processStart Discord.exe\n"
             "      3.5 s CPU and 45.0 MiB of disk in the first 2 minutes after logon "
             "(median of 5 logons)\n");
  expect_has(text, "[2] off  Print Spooler (service, machine), start auto (delayed)");
  expect_has(text, "      impact not known\n");
  expect_has(text, "[3] once Setup (runonce, user)");
  expect_has(text, "delayed a boot by up to 4.2 s (Windows diagnostics, last 30 days)");

  text = both_forms({"startup", "boot-times"}, boots);
  expect_has(text,
             "Latest boot 2026-11-11 12:00:00 UTC: boot 35.2 s (+3.0 s from the median) "
             "(main path 20.1 s, post-boot 15.1 s), logon 12.3 s (-1.0 s from the median)");
  expect_has(text, "discord.exe");
  expect_has(text, "CPU 3.5 s, disk 45.0 MiB, startup item Discord (run, user)");
  expect_has(text, "Median over 9 boots of the last 30 days: boot 32.2 s, logon 13.3 s");
  expect_has(text,
             "Plan 4 applied 2026-11-11 12:00:00 UTC disabled Teams (run, user)\n"
             "  logon before 20.0 s (5 boots), after not measured yet");
}

const Json plan_summary{{"id", 12},
                        {"title", "Clear the npm cache"},
                        {"kind", "cleanup"},
                        {"tier", "ask"},
                        {"scope", "service"},
                        {"state", "awaiting_approval"},
                        {"proposer", "mcp:claude"},
                        {"created_at", noon_seconds},
                        {"items", 2},
                        {"bytes", 1073741824},
                        {"needs_restart", false},
                        {"verification", nullptr}};

TEST_F(CliReadTest, PlansListSendsTheStateAndPlansGetShowsOnePlan) {
  const Json list{{"plans", Json::array({plan_summary})}};
  Json plan = plan_summary;
  plan["items"] = Json::array({Json{{"id", 30},
                                    {"path", R"(C:\Users\owner\AppData\Local\npm-cache\a)"},
                                    {"bytes", 536870912},
                                    {"rule", "npm/cache"},
                                    {"reason", "npm's download cache"},
                                    {"approved", nullptr}},
                               Json{{"id", 31},
                                    {"path", R"(C:\Users\owner\AppData\Local\npm-cache\b)"},
                                    {"bytes", 536870912},
                                    {"rule", nullptr},
                                    {"reason", "npm's download cache"},
                                    {"approved", false}}});
  plan["items_total"] = 5;
  plan["commands"] = Json::array({"quarantine 2 files"});
  plan["undo"] = "restore from the quarantine for 7 days";
  plan["warnings"] = Json::array({"npm downloads again"});
  plan["params"] = Json::object();
  plan["freed"] = nullptr;
  plan["detail"] = nullptr;
  serve(ipc::plans_list_method, {Param{.name = "state", .type = ParamType::token}}, list);
  serve(ipc::plans_get_method, {Param{.name = "id", .type = ParamType::integer, .required = true}},
        plan);

  std::string text = both_forms({"plans", "list", "--state", "awaiting_approval"}, list);
  EXPECT_EQ(sent(ipc::plans_list_method), (Json{{"state", "awaiting_approval"}}));
  expect_has(text,
             "[12] awaiting_approval cleanup        Clear the npm cache: 2 items, 1.0 GiB, "
             "by mcp:claude, 2026-11-11 12:00:00 UTC\n");
  static_cast<void>(cli({"plans", "list"}));
  EXPECT_EQ(sent(ipc::plans_list_method), Json::object());

  text = both_forms({"plans", "get", "12"}, plan);
  EXPECT_EQ(sent(ipc::plans_get_method), (Json{{"id", 12}}));
  expect_has(text,
             "Plan 12: Clear the npm cache\n  cleanup (ask tier, runs in the service), "
             "awaiting_approval, proposed by mcp:claude 2026-11-11 12:00:00 UTC\n"
             "  5 items, 1.0 GiB\n  runs: quarantine 2 files\n"
             "  undo: restore from the quarantine for 7 days\n"
             "  warning: npm downloads again\n  verification: not applied yet\n");
  expect_has(text,
             "[30]  512.0 MiB  C:\\Users\\owner\\AppData\\Local\\npm-cache\\a\n"
             "        rule npm/cache, npm's download cache; not decided\n");
  expect_has(text, "        npm's download cache; rejected\n");
  expect_has(text, "    ... and 3 more items not listed\n");
}

TEST_F(CliReadTest, PlansGetOfAnUnknownPlanFails) {
  refuse(ipc::plans_get_method, {Param{.name = "id", .type = ParamType::integer, .required = true}},
         Errc::not_found, "plan 99 does not exist");
  const Outcome human = cli({"plans", "get", "99"});
  EXPECT_EQ(human.code, exit_unavailable);
  EXPECT_TRUE(human.out.empty());
  expect_has(human.err, "plan 99 does not exist");
  const Outcome json = cli({"plans", "get", "99", "--json"});
  EXPECT_EQ(json.code, exit_unavailable);
  EXPECT_TRUE(Json::parse(json.out).contains("error")) << json.out;
}

TEST_F(CliReadTest, QuarantineListsEachRunAndItsItems) {
  const Json runs{
      {"runs", Json::array({Json{{"plan", 12},
                                 {"moved_at", noon_seconds},
                                 {"purge_after", noon_seconds + (7 * day)},
                                 {"bytes", 1048576},
                                 {"items", Json::array({Json{{"item", 30},
                                                             {"original", R"(C:\cache\a)"},
                                                             {"location", R"(C:\MyClean\q\12\a)"},
                                                             {"bytes", 1048576}}})}}})}};
  serve(ipc::quarantine_list_method, {}, runs);
  const std::string text = both_forms({"quarantine"}, runs);
  expect_has(text,
             "plan 12, moved 2026-11-11 12:00:00 UTC, purged after 2026-11-18 12:00:00 UTC, "
             "1.0 MiB\n    [30]    1.0 MiB  C:\\cache\\a\n        held at C:\\MyClean\\q\\12\\a\n");
}

TEST(CliReadUsageTest, StartupPlansAndQuarantineUsageErrorsExitThree) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{{"startup"},
                                             {"startup", "frobnicate"},
                                             {"plans"},
                                             {"plans", "get"},
                                             {"plans", "get", "twelve"},
                                             {"plans", "get", "0"},
                                             {"plans", "list", "extra"},
                                             {"quarantine", "extra"}}) {
    const Outcome outcome = usage_of(args);
    EXPECT_EQ(outcome.code, exit_usage) << args.back();
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_FALSE(outcome.err.empty());
  }
}

// ---- resident, freezes pauses, governor events, alerts, cleaning, system space, reports ---------

TEST_F(CliReadTest, ResidentCostsAndTrendOfOneProgram) {
  const Json costs{{"days", 30},
                   {"from", noon_seconds - (30 * day)},
                   {"programs", Json::array({Json{{"image", "chrome.exe"},
                                                  {"key", "chrome.exe"},
                                                  {"hours_running", 600.5},
                                                  {"cpu_seconds", 3600},
                                                  {"ram_average", 1288490188},
                                                  {"ram_peak", 2147483648},
                                                  {"disk_bytes", 53687091200},
                                                  {"net_bytes", 3221225472}}})}};
  const Json trend{
      {"image", "leaky.exe"},
      {"key", "leaky.exe"},
      {"points", Json::array({Json{{"time", noon_seconds - 3600}, {"private_bytes", 104857600}},
                              Json{{"time", noon_seconds - 1800}, {"private_bytes", 52428800}},
                              Json{{"time", noon_seconds}, {"private_bytes", 209715200}}})},
      {"leak", {{"hours", 2}, {"hourly", Json::array({104857600, 209715200})}}}};
  serve(ipc::resident_costs_method, {}, costs);
  serve(ipc::resident_trend_method,
        {Param{.name = "key", .type = ParamType::token, .required = true}}, trend);

  std::string text = both_forms({"resident", "costs"}, costs);
  expect_has(text, "Resident programs of the last 30 days (since 2026-10-12 12:00:00 UTC)");
  expect_has(text,
             "  chrome.exe (key chrome.exe): 600.5 h running, CPU 3600 s\n      RAM 1.2 GiB "
             "average, 2.0 GiB peak; disk 50.0 GiB, network 3.0 GiB\n");

  text = both_forms({"resident", "trend", "leaky.exe"}, trend);
  EXPECT_EQ(sent(ipc::resident_trend_method), (Json{{"key", "leaky.exe"}}));
  expect_has(text,
             "Private bytes of leaky.exe (key leaky.exe) over the last 48 hours, 3 points\n"
             "  100.0 MiB at 2026-11-11 11:00:00 UTC, 200.0 MiB at 2026-11-11 12:00:00 UTC; "
             "lowest 50.0 MiB, highest 200.0 MiB\n"
             "  Steady growth for 2 hours; hourly medians: 100.0 MiB, 200.0 MiB\n");
}

TEST_F(CliReadTest, FreezesPausesShowsTheSuspectsTheOfferAndThePauses) {
  const Json rate{{"incidents", 12}, {"hours", 168.0}, {"per_day", 1.7}};
  const Json pauses{
      {"collecting", false},
      {"baseline", rate},
      {"suspects", Json::array({Json{{"id", 1},
                                     {"image", "PowerToys.KeyboardManagerEngine.exe"},
                                     {"hook", true},
                                     {"mode", "guided"},
                                     {"running", true}},
                                Json{{"id", 2},
                                     {"image", "ditto.exe"},
                                     {"hook", false},
                                     {"mode", "suspend"},
                                     {"running", nullptr}}})},
      {"offer", {{"suspect", 2}, {"image", "ditto.exe"}, {"mode", "suspend"}, {"hours", 24}}},
      {"active", nullptr},
      {"pauses",
       Json::array({Json{{"plan", 8},
                         {"image", "ditto.exe"},
                         {"mode", "suspend"},
                         {"started", noon_seconds - 86400},
                         {"ends", noon_seconds},
                         {"restored", noon_seconds},
                         {"end_reason", "period_ended"},
                         {"baseline", rate},
                         {"during", {{"incidents", 0}, {"hours", 24.0}, {"per_day", 0.0}}},
                         {"change", "lower"}}})}};
  serve(ipc::input_pauses_method, {}, pauses);
  const std::string text = both_forms({"freezes", "pauses"}, pauses);
  expect_has(text, "Baseline (the 7 days before now): 12 incidents in 168.0 h (1.7 a day)");
  expect_has(text,
             "[1] PowerToys.KeyboardManagerEngine.exe (guided, owns a keyboard hook), running\n");
  expect_has(text, "[2] ditto.exe (suspend)\n");
  expect_has(text, "Offered: pause ditto.exe (suspect 2, suspend) for 24 h");
  expect_has(text,
             "plan 8 ditto.exe (suspend), 2026-11-10 12:00:00 UTC to 2026-11-11 12:00:00 UTC, "
             "period_ended\n      during: 0 incidents in 24.0 h (0.0 a day); baseline: 12 "
             "incidents in 168.0 h (1.7 a day); lower\n");
}

TEST_F(CliReadTest, GovernorEventsSendsTheHoursAsSinceAndTheLimit) {
  const Json events{{"events", Json::array({Json{{"id", 3},
                                                 {"time", noon_seconds},
                                                 {"kind", "apply_step"},
                                                 {"root", {{"pid", 100}, {"created", 1}}},
                                                 {"image", "cargo.exe"},
                                                 {"class", "build"},
                                                 {"step", "ecoqos"},
                                                 {"reason", "cpu"},
                                                 {"actor", "governor"},
                                                 {"members", 4},
                                                 {"metrics", nullptr},
                                                 {"detail", ""}}})}};
  serve(ipc::governor_events_method,
        {Param{.name = "since", .type = ParamType::integer},
         Param{.name = "limit", .type = ParamType::integer}},
        events);
  const std::string text =
      both_forms({"governor", "events", "--hours", "2", "--limit", "50"}, events);
  EXPECT_EQ(sent(ipc::governor_events_method),
            (Json{{"since", noon_seconds - 7200}, {"limit", 50}}));
  expect_has(text,
             "Governor events, newest first:\n  2026-11-11 12:00:00 UTC apply_step ecoqos "
             "cargo.exe (pid 100): cpu, governor\n");
  static_cast<void>(cli({"governor", "events"}));
  EXPECT_EQ(sent(ipc::governor_events_method), Json::object());
}

TEST_F(CliReadTest, AlertsSendsTheHoursAsSinceAndTheBadgeCounts) {
  const Json list{{"alerts", Json::array()}};
  const Json badge{{"count", 2}, {"worst", "warning"}};
  serve(ipc::alerts_list_method, {Param{.name = "since", .type = ParamType::integer}}, list);
  serve(ipc::alerts_badge_method, {}, badge);
  expect_has(both_forms({"alerts", "--hours", "24"}, list), "Alerts:\n  none\n");
  EXPECT_EQ(sent(ipc::alerts_list_method), (Json{{"since", noon_seconds - 86400}}));
  static_cast<void>(cli({"alerts"}));
  EXPECT_EQ(sent(ipc::alerts_list_method), Json::object());
  EXPECT_EQ(both_forms({"alerts", "badge"}, badge),
            "Alerts: 2 active and not snoozed, worst warning\n");
}

TEST_F(CliReadTest, CleaningAndSystemSpaceShowWhatTheServiceReports) {
  const Json status{{"state", "scanning"},
                    {"progress", 0.45},
                    {"last_completed", noon_seconds},
                    {"candidates", 12},
                    {"bytes", 2147483648},
                    {"next_auto_run", nullptr},
                    {"purging", Json::array({7})},
                    {"applying",
                     {{"kind", "plan"},
                      {"plan", 12},
                      {"title", "Clear the npm cache"},
                      {"started_at", noon_seconds},
                      {"items_done", 1},
                      {"items_total", 2},
                      {"bytes_done", 536870912},
                      {"bytes_total", 1073741824},
                      {"files_done", 40},
                      {"current", R"(C:\cache\b)"}}},
                    {"unavailable", nullptr}};
  const Json space{{"settings", Json::array({Json{{"kind", "hibernation"},
                                                  {"current", "full"},
                                                  {"cost", 13743895347},
                                                  {"recommended", "reduced"},
                                                  {"restart_required", false},
                                                  {"error", nullptr}},
                                             Json{{"kind", "page_file"},
                                                  {"current", "system-managed on C"},
                                                  {"cost", 17179869184},
                                                  {"recommended", nullptr},
                                                  {"restart_required", false},
                                                  {"error", nullptr}},
                                             Json{{"kind", "shadow_storage"},
                                                  {"current", nullptr},
                                                  {"cost", nullptr},
                                                  {"recommended", nullptr},
                                                  {"restart_required", false},
                                                  {"error", "access denied"}}})}};
  serve(ipc::cleaning_status_method, {}, status);
  serve(ipc::system_space_status_method, {}, space);

  std::string text = both_forms({"cleaning"}, status);
  expect_has(text,
             "Cleaning: scanning (45 %)\n  last scan completed 2026-11-11 12:00:00 UTC, next "
             "automatic run not planned\n  12 candidates, 2.0 GiB\n  Applying plan 12 \"Clear "
             "the npm cache\" since 2026-11-11 12:00:00 UTC: 1 of 2 items, 512.0 MiB of 1.0 "
             "GiB, 40 files\n  now: C:\\cache\\b\n  quarantine runs queued for purging: 7\n");

  text = both_forms({"system-space"}, space);
  expect_has(text, "  hibernation: full, 12.8 GiB on disk; recommended: reduced\n");
  expect_has(text, "  page_file: system-managed on C, 16.0 GiB on disk; no change recommended\n");
  expect_has(text, "  shadow_storage: could not be read: access denied\n");
}

TEST_F(CliReadTest, ReportListShowsEachMonthAndItsFiles) {
  const Json list{
      {"reports",
       Json::array({Json{{"month", "2026-11"},
                         {"snapshot", "2026-11"},
                         {"generated_at", noon_seconds},
                         {"against", "2026-10"},
                         {"first_report", false},
                         {"files",
                          {{"markdown", R"(C:\ProgramData\MyClean\reports\2026-11\report.md)"},
                           {"html", R"(C:\ProgramData\MyClean\reports\2026-11\report.html)"}}}},
                    Json{{"month", "2026-10"},
                         {"snapshot", "2026-10"},
                         {"generated_at", noon_seconds - (31 * day)},
                         {"against", nullptr},
                         {"first_report", true},
                         {"files", {{"markdown", "a.md"}, {"html", "a.html"}}}}})}};
  serve(ipc::reports_list_method, {}, list);
  const std::string text = both_forms({"report", "list"}, list);
  expect_has(text,
             "2026-11  compared with 2026-10\n    Markdown: "
             "C:\\ProgramData\\MyClean\\reports\\2026-11\\report.md\n    HTML: "
             "C:\\ProgramData\\MyClean\\reports\\2026-11\\report.html\n");
  expect_has(text, "2026-10  the first report\n");
}

TEST(CliReadUsageTest, WatcherUsageErrorsExitThree) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{{"resident"},
                                             {"resident", "trend"},
                                             {"resident", "trend", R"(C:\x.exe)"},
                                             {"resident", "frobnicate"},
                                             {"governor", "events", "--hours", "two"},
                                             {"governor", "events", "--limit", "many"},
                                             {"governor", "frobnicate"},
                                             {"freezes", "pauses", "extra"},
                                             {"alerts", "--hours", "x"},
                                             {"cleaning", "extra"},
                                             {"system-space", "extra"},
                                             {"report", "list", "extra"}}) {
    const Outcome outcome = usage_of(args);
    EXPECT_EQ(outcome.code, exit_usage) << args.back();
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_FALSE(outcome.err.empty());
  }
}

// ---- apps, backups, security --------------------------------------------------------------------

TEST_F(CliReadTest, AppsSendsTheUnusedDaysAndListsEachApp) {
  const Json list{{"count", 40},
                  {"history", {{"days", 90}}},
                  {"apps", Json::array({Json{{"id", 1},
                                             {"name", "Old Tool"},
                                             {"publisher", "Tools Ltd"},
                                             {"description", "unknown"},
                                             {"last_used", {{"text", "2026-05-10 (Prefetch)"}}},
                                             {"verdict", "unknown"},
                                             {"reason", "not in MyClean's knowledge base"}}})},
                  {"problems", Json::array()}};
  serve(ipc::apps_list_method, {Param{.name = "unused_days", .type = ParamType::integer}}, list);
  const std::string text = both_forms({"apps", "--unused-days", "180"}, list);
  EXPECT_EQ(sent(ipc::apps_list_method), (Json{{"unused_days", 180}}));
  expect_has(text,
             "Installed apps (1 shown of 40; MyClean's history covers 90 days):\n"
             "    1 [unknown] Old Tool (Tools Ltd): last used 2026-05-10 (Prefetch)\n");
  static_cast<void>(cli({"apps"}));
  EXPECT_EQ(sent(ipc::apps_list_method), Json::object());
}

TEST_F(CliReadTest, BackupsSendsRefreshOnlyWhenAsked) {
  const Json check{{"state", "ready"},
                   {"checked_at", noon_seconds},
                   {"error", nullptr},
                   {"summary", "the newest backup is 2 days old"},
                   {"sources", Json::array({Json{{"name", "File History"},
                                                 {"status", "found"},
                                                 {"location", R"(K:\FileHistory)"},
                                                 {"newest", noon_seconds - (2 * day)},
                                                 {"detail", nullptr}}})},
                   {"repositories", nullptr},
                   {"recommendation", nullptr}};
  serve(ipc::backups_check_method, {refresh}, check);
  const std::string text = both_forms({"backups"}, check);
  EXPECT_EQ(sent(ipc::backups_check_method), (Json{{"refresh", false}}));
  expect_has(text,
             "Backups, checked 2026-11-11 12:00:00 UTC: the newest backup is 2 days old\n"
             "  File History: found, K:\\FileHistory, newest 2026-11-09 12:00:00 UTC\n");
  static_cast<void>(cli({"backups", "--refresh"}));
  EXPECT_EQ(sent(ipc::backups_check_method), (Json{{"refresh", true}}));
}

TEST_F(CliReadTest, SecurityScanExtensionsNetworkAndDefenderScan) {
  const Json scan{
      {"items",
       Json::array({Json{{"id", 4},
                         {"source", "run"},
                         {"location", R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)"},
                         {"name", "Updater"},
                         {"command", R"(C:\Users\owner\AppData\updater.exe)"},
                         {"status", "suspicious"},
                         {"summary", "suspicious because unsigned, user-writable-path"}}})},
      {"defender",
       {{"available", false}, {"error", "Defender is off"}, {"detections", Json::array()}}},
      {"problems", Json::array({"WMI: access denied"})}};
  const Json extensions{
      {"extensions", Json::array({Json{{"browser", "edge"},
                                       {"profile", "Default"},
                                       {"id", "abc"},
                                       {"name", "Coupons"},
                                       {"origin", "external"},
                                       {"flag", "probably not installed by you"}}})},
      {"problems", Json::array()}};
  const Json network{
      {"hours", 2.5},
      {"processes",
       Json::array({Json{{"pid", 300},
                         {"image", "agent.exe"},
                         {"sent", 1000},
                         {"received", 2000},
                         {"visible_window", false},
                         {"remotes", Json::array({Json{{"address", "203.0.113.5"},
                                                       {"port", 443},
                                                       {"name", "example.org"}}})}}})}};
  const Json started{{"started", true}, {"image", R"(C:\Users\owner\AppData\updater.exe)"}};
  serve(ipc::security_scan_method, {}, scan);
  serve(ipc::security_extensions_method, {}, extensions);
  serve(ipc::security_network_method, {Param{.name = "hours", .type = ParamType::number}}, network);
  serve(ipc::security_defender_scan_method,
        {Param{.name = "item", .type = ParamType::integer, .required = true}}, started);

  std::string text = both_forms({"security", "scan"}, scan);
  expect_has(text,
             "[4] run  Updater\n    HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\n"
             "    runs C:\\Users\\owner\\AppData\\updater.exe\n    suspicious because "
             "unsigned, user-writable-path\n");
  expect_has(text, "Microsoft Defender: Defender is off\n");
  expect_has(text, "not read: WMI: access denied\n");

  text = both_forms({"security", "extensions"}, extensions);
  expect_has(text, "edge/Default  Coupons (abc)  external  probably not installed by you\n");

  text = both_forms({"security", "network", "--hours", "2.5"}, network);
  EXPECT_EQ(sent(ipc::security_network_method), (Json{{"hours", 2.5}}));
  expect_has(text,
             "agent.exe (pid 300)  sent 1000 B, received 2000 B, no visible window\n"
             "    203.0.113.5:443 example.org\n");

  text = both_forms({"security", "defender-scan", "4"}, started);
  EXPECT_EQ(sent(ipc::security_defender_scan_method), (Json{{"item", 4}}));
  expect_has(text, R"(Defender scan started for C:\Users\owner\AppData\updater.exe)");
}

TEST(CliReadUsageTest, AppsAndSecurityUsageErrorsExitThree) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{{"apps", "--unused-days", "soon"},
                                             {"apps", "extra"},
                                             {"backups", "extra"},
                                             {"security", "frobnicate"},
                                             {"security", "network", "--hours", "x"},
                                             {"security", "defender-scan", "0"},
                                             {"security", "defender-scan", "four"}}) {
    const Outcome outcome = usage_of(args);
    EXPECT_EQ(outcome.code, exit_usage) << args.back();
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_FALSE(outcome.err.empty());
  }
}

}  // namespace
}  // namespace myclean::cli
