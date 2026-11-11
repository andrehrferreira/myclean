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
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
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

}  // namespace
}  // namespace myclean::cli
