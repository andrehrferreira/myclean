#include "cli/cli.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "fakes/fake_clock.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ipc/security.hpp"
#include "svc/fake_machine.hpp"
#include "svc/phase1_fakes.hpp"
#include "svc/service.hpp"
#include "svc/svc_test_support.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

struct Outcome {
  int code = -1;
  std::string out;
  std::string err;
};

// The name to pass as --pipe for a full pipe path.
std::string pipe_token(const std::wstring& pipe) {
  std::string name;
  for (const wchar_t c : pipe.substr(pipe.rfind(L'\\') + 1)) {
    name.push_back(static_cast<char>(c));  // test pipe names are ASCII
  }
  return name;
}

// A pipe nothing listens on.
std::string nobody() {
  return "myclean-cli-test-nobody-" + std::to_string(::GetCurrentProcessId());
}

Outcome run_cli(const std::vector<std::string>& args, const std::string& input = {},
                std::chrono::milliseconds timeout = 5s) {
  std::istringstream in(input);
  std::ostringstream out;
  std::ostringstream err;
  const int code = run(args, in, out, err, timeout);
  return Outcome{.code = code, .out = out.str(), .err = err.str()};
}

// The single JSON document `out` must hold.
Json document(const Outcome& outcome) {
  EXPECT_TRUE(outcome.out.ends_with("\n")) << outcome.out;
  const Json json = Json::parse(outcome.out, nullptr, false);
  EXPECT_FALSE(json.is_discarded()) << outcome.out;
  return json;
}

// MCP responses, one JSON document per line.
std::vector<Json> responses(const std::string& out) {
  std::vector<Json> result;
  std::istringstream lines(out);
  for (std::string line; std::getline(lines, line);) {
    result.push_back(Json::parse(line));
  }
  return result;
}

// The CLI against a console-mode service in this process, running the telemetry component over
// the fake machine on a pipe of its own.
class CliTest : public ::testing::Test {
 protected:
  void start(svc::testing::FakeMachine machine = svc::testing::fake_machine()) {
    Result<std::unique_ptr<svc::Service>> started =
        svc::testing::start_telemetry_service(std::move(machine), dir_.path(), pipe_, clock_);
    ASSERT_TRUE(started.has_value()) << started.error().message;
    service_ = std::move(*started);
  }

  [[nodiscard]] Outcome cli(std::vector<std::string> args, const std::string& input = {}) const {
    args.insert(args.begin(), {"--pipe", pipe_token(pipe_)});
    return run_cli(args, input);
  }

  [[nodiscard]] const std::wstring& pipe() const { return pipe_; }

 private:
  svc::testing::TempDir dir_;
  fakes::FakeClock clock_{noon};
  std::wstring pipe_ = svc::testing::unique_pipe_name();
  std::unique_ptr<svc::Service> service_;
};

TEST_F(CliTest, StatusJsonExitsZeroWhenTheServiceIsHealthy) {
  start();
  const Outcome outcome = cli({"status", "--json"});
  EXPECT_EQ(outcome.code, exit_ok) << outcome.err;
  const Json status = document(outcome);
  EXPECT_EQ(status.at("health"), "ok");
  EXPECT_FALSE(status.contains("reason"));
  EXPECT_EQ(status.at("component_health")[0].at("name"), "telemetry");

  const Outcome human = cli({"status"});
  EXPECT_EQ(human.code, exit_ok);
  EXPECT_TRUE(human.out.starts_with("MyClean service: ok")) << human.out;
}

TEST_F(CliTest, StatusJsonExitsTwoWithTheReasonWhenDegraded) {
  svc::testing::FakeMachine machine = svc::testing::fake_machine();
  machine.volumes->fail_with("the device is not ready");
  start(std::move(machine));
  const Outcome outcome = cli({"status", "--json"});
  EXPECT_EQ(outcome.code, exit_degraded);
  const Json status = document(outcome);
  EXPECT_EQ(status.at("health"), "degraded");
  EXPECT_EQ(status.at("reason"), "degraded: telemetry: volumes: the device is not ready");
}

TEST(CliStatusTest, StatusJsonReportsAServiceThatIsNotRunning) {
  const auto started = std::chrono::steady_clock::now();
  const Outcome outcome = run_cli({"--pipe", nobody(), "status", "--json"});
  EXPECT_LT(std::chrono::steady_clock::now() - started, 20s);
  EXPECT_EQ(outcome.code, exit_unavailable);
  const Json status = document(outcome);
  EXPECT_EQ(status.at("health"), "unreachable");
  EXPECT_NE(status.at("reason").get<std::string>().find("not running"), std::string::npos);
}

TEST(CliStatusTest, StatusJsonGivesUpOnAServiceThatNeverAnswers) {
  const Result<std::string> owner = platform::current_user_sid();
  ASSERT_TRUE(owner.has_value());
  const std::wstring pipe = svc::testing::unique_pipe_name();
  std::promise<void> release;
  const std::shared_future<void> released = release.get_future().share();
  Result<std::unique_ptr<platform::PipeServer>> hung =
      platform::PipeServer::start(platform::PipeServerOptions{.name = pipe,
                                                              .owner_sid = *owner,
                                                              .handler =
                                                                  [released](std::string_view) {
                                                                    released.wait();
                                                                    return std::string();
                                                                  },
                                                              .on_error = {}});
  ASSERT_TRUE(hung.has_value()) << hung.error().message;

  const auto started = std::chrono::steady_clock::now();
  const Outcome outcome = run_cli({"--pipe", pipe_token(pipe), "status", "--json"}, {}, 1s);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 20s);
  release.set_value();
  EXPECT_EQ(outcome.code, exit_unavailable);
  const Json status = document(outcome);
  EXPECT_EQ(status.at("health"), "unreachable");
  EXPECT_TRUE(status.at("reason").get<std::string>().starts_with("timeout: ")) << status.dump();
}

TEST_F(CliTest, HealthPrintsOneJsonDocumentOrAReadableSummary) {
  start();
  const Outcome json = cli({"health", "--json"});
  EXPECT_EQ(json.code, exit_ok) << json.err;
  const Json snapshot = document(json);
  ASSERT_EQ(snapshot.at("trees").size(), 1U);
  EXPECT_EQ(snapshot.at("trees")[0].at("image"), "editor.exe");
  EXPECT_TRUE(snapshot.at("trees")[0].at("trust").is_null());
  EXPECT_EQ(snapshot.at("signals").at("system_volume"), "C:");
  EXPECT_EQ(snapshot.at("signals").at("commit_percent"), 50.0);
  EXPECT_TRUE(json.err.empty());

  const Outcome human = cli({"health"});
  EXPECT_EQ(human.code, exit_ok);
  EXPECT_NE(human.out.find("Process trees above 5 % CPU or 5 % of memory (1 of 3 trees)"),
            std::string::npos)
      << human.out;
  EXPECT_NE(human.out.find("editor.exe"), std::string::npos);
  EXPECT_NE(human.out.find("C:"), std::string::npos);
  EXPECT_EQ(human.out.find("System Idle Process"), std::string::npos);
}

TEST_F(CliTest, EveryReadCommandAnswersInBothForms) {
  start();
  const std::vector<std::vector<std::string>> commands{
      {"processes", "--sort", "memory", "--limit", "2"},
      {"volumes"},
      {"metrics", "latest"},
      {"metrics", "query", "memory.total", "--hours", "0.5", "--max-points", "10"},
      {"metrics", "query", "volume.free", "--entity", "C"}};
  for (std::vector<std::string> command : commands) {
    const Outcome human = cli(command);
    EXPECT_EQ(human.code, exit_ok) << command.front() << ": " << human.err;
    EXPECT_FALSE(human.out.empty());
    command.emplace_back("--json");
    const Outcome json = cli(command);
    EXPECT_EQ(json.code, exit_ok) << command.front() << ": " << json.err;
    static_cast<void>(document(json));
  }
  const Json top = document(cli({"processes", "--sort", "memory", "--limit", "2", "--json"}));
  EXPECT_EQ(top.at("trees").size(), 2U);
  EXPECT_EQ(top.at("trees")[0].at("image"), "editor.exe");
  const Json volume = document(cli({"metrics", "query", "volume.free", "--entity", "C", "--json"}));
  EXPECT_EQ(volume.at("entity"), "C:");
}

constexpr std::string_view initialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18",)"
    R"("capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";

// The CLI against a service with every phase-1 component over fakes (svc/phase1_fakes.hpp).
class CliPhase1Test : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<svc::Service>> started =
        svc::testing::start_phase1_service(fakes_, dir_.path(), pipe_, clock_);
    ASSERT_TRUE(started.has_value()) << started.error().message;
    service_ = std::move(*started);
  }

  [[nodiscard]] Outcome cli(std::vector<std::string> args, const std::string& input = {}) const {
    args.insert(args.begin(), {"--pipe", pipe_token(pipe_)});
    return run_cli(args, input);
  }

 private:
  svc::testing::TempDir dir_;
  fakes::FakeClock clock_{noon};
  std::wstring pipe_ = svc::testing::unique_pipe_name();
  svc::testing::Phase1Fakes fakes_;
  std::unique_ptr<svc::Service> service_;
};

TEST_F(CliPhase1Test, NewReadCommandsAnswerInBothForms) {
  ASSERT_TRUE(svc::testing::eventually(
      [&] { return document(cli({"disks", "--json"})).at("drives").size() == 2; }));
  for (std::vector<std::string> command : std::vector<std::vector<std::string>>{
           {"explain", "101"}, {"disks"}, {"alerts"}, {"freezes"}, {"health"}}) {
    const Outcome human = cli(command);
    EXPECT_EQ(human.code, exit_ok) << command.front() << ": " << human.err;
    EXPECT_FALSE(human.out.empty());
    command.emplace_back("--json");
    const Outcome json = cli(command);
    EXPECT_EQ(json.code, exit_ok) << command.front() << ": " << json.err;
    static_cast<void>(document(json));
  }

  const Json explained = document(cli({"explain", "101", "--json"}));
  EXPECT_EQ(explained.at("tree").at("pid"), 100);
  const std::string explain_text = cli({"explain", "101"}).out;
  EXPECT_NE(explain_text.find("trust: signed by Microsoft Corporation"), std::string::npos)
      << explain_text;
  EXPECT_NE(explain_text.find("knowledge base: Microsoft PowerToys"), std::string::npos);

  const std::string disks = cli({"disks"}).out;
  EXPECT_NE(disks.find("Samsung SSD 980 PRO 2TB"), std::string::npos) << disks;
  EXPECT_NE(disks.find("not readable over USB"), std::string::npos) << disks;
  EXPECT_NE(cli({"freezes"}).out.find("No freeze over 250 ms"), std::string::npos);

  const std::string health = cli({"health"}).out;
  EXPECT_NE(health.find("UNRECOGNIZED (not-in-knowledge-base, unsigned, user-writable-path)"),
            std::string::npos)
      << health;
  EXPECT_EQ(health.find("malicious"), std::string::npos);
  EXPECT_EQ(health.find("dangerous"), std::string::npos);
  const Json snapshot = document(cli({"health", "--json"}));
  ASSERT_EQ(snapshot.at("unrecognized").size(), 1U);
  EXPECT_EQ(snapshot.at("unrecognized")[0].at("pid"), 200);
}

TEST_F(CliPhase1Test, ExplainOfAMissingProcessFails) {
  const Outcome gone = cli({"explain", "99999"});
  EXPECT_EQ(gone.code, exit_unavailable);
  EXPECT_NE(gone.err.find("process 99999 was not found"), std::string::npos) << gone.err;
  const Outcome json = cli({"explain", "99999", "--json"});
  EXPECT_EQ(json.code, exit_unavailable);
  EXPECT_TRUE(document(json).contains("error"));
}

TEST_F(CliPhase1Test, McpExplainsAProcess) {
  const std::string input =
      std::string(initialize) + "\n" +
      R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"explain_process","arguments":{"pid":200}}})" +
      "\n" +
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"explain_process","arguments":{"pid":99999}}})" +
      "\n";
  const std::vector<Json> answers = responses(cli({"mcp"}, input).out);
  ASSERT_EQ(answers.size(), 3U);
  EXPECT_EQ(answers[1].at("result").at("isError"), false);
  const Json editor =
      Json::parse(answers[1].at("result").at("content")[0].at("text").get<std::string>());
  EXPECT_EQ(editor.at("trust").at("unrecognized"), true);
  EXPECT_TRUE(editor.at("kb").is_null());
  EXPECT_EQ(answers[2].at("result").at("isError"), true);
  EXPECT_NE(answers[2].at("result").at("content")[0].at("text").get<std::string>().find(
                "process 99999 was not found"),
            std::string::npos);
}

TEST(CliStatusTest, ReadCommandsSayWhatNeedsTheService) {
  const Outcome human = run_cli({"--pipe", nobody(), "health"});
  EXPECT_EQ(human.code, exit_unavailable);
  EXPECT_TRUE(human.out.empty());
  EXPECT_NE(human.err.find("not reachable"), std::string::npos) << human.err;
  EXPECT_NE(human.err.find("live telemetry"), std::string::npos) << human.err;

  const Outcome json = run_cli({"--pipe", nobody(), "metrics", "latest", "--json"});
  EXPECT_EQ(json.code, exit_unavailable);
  EXPECT_EQ(document(json).at("health"), "unreachable");
}

TEST(CliStatusTest, UsageErrorsExitThree) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{{},
                                             {"bogus"},
                                             {"--json"},
                                             {"status", "extra"},
                                             {"status", "--frobnicate", "1"},
                                             {"--pipe", R"(C:\Windows)", "status"},
                                             {"processes", "--limit", "ten"},
                                             {"metrics", "query"},
                                             {"explain"},
                                             {"explain", "notepad"},
                                             {"explain", "-4"},
                                             {"disks", "extra"},
                                             {"metrics", "query", "cpu.utility", "--hours"}}) {
    const Outcome outcome = run_cli(args);
    EXPECT_EQ(outcome.code, exit_usage) << (args.empty() ? "(none)" : args.front());
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_FALSE(outcome.err.empty());
  }
}

TEST_F(CliTest, McpAnswersFromTheService) {
  start();
  const std::string input =
      std::string(initialize) + "\n" + R"({"jsonrpc":"2.0","method":"notifications/initialized"})" +
      "\n" +
      R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"health_snapshot"}})" +
      "\n" + R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_volumes"}})" +
      "\n";
  const Outcome outcome = cli({"mcp"}, input);
  EXPECT_EQ(outcome.code, exit_ok) << outcome.err;
  const std::vector<Json> answers = responses(outcome.out);
  ASSERT_EQ(answers.size(), 3U);
  EXPECT_EQ(answers[0].at("result").at("serverInfo").at("name"), "myclean");
  const Json& snapshot = answers[1].at("result");
  EXPECT_EQ(snapshot.at("isError"), false);
  const Json health = Json::parse(snapshot.at("content")[0].at("text").get<std::string>());
  EXPECT_EQ(health.at("trees")[0].at("pid"), 200);
  EXPECT_EQ(answers[2].at("result").at("isError"), false);
}

TEST(CliStatusTest, McpKeepsRunningWhileTheServiceIsDown) {
  const std::string input =
      std::string(initialize) + "\n" +
      R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"health_snapshot"}})" +
      "\n" + R"({"jsonrpc":"2.0","id":3,"method":"ping"})" + "\n";
  const Outcome outcome = run_cli({"--pipe", nobody(), "mcp"}, input);
  EXPECT_EQ(outcome.code, exit_ok);
  const std::vector<Json> answers = responses(outcome.out);
  ASSERT_EQ(answers.size(), 3U);
  const Json& snapshot = answers[1].at("result");
  EXPECT_EQ(snapshot.at("isError"), true);
  const std::string text = snapshot.at("content")[0].at("text").get<std::string>();
  EXPECT_NE(text.find("live telemetry"), std::string::npos) << text;
  EXPECT_NE(text.find("disk health"), std::string::npos) << text;
  EXPECT_EQ(answers[2].at("id"), 3);
}

struct Spawned {
  DWORD code = 0;
  std::string out;
};

// Runs the built myclean.exe without a window and returns its exit code and stdout.
Spawned spawn_cli(const std::vector<std::string>& args) {
  std::wstring command =
      L"\"" + std::filesystem::path(MYCLEAN_CLI_EXE).make_preferred().wstring() + L"\"";
  for (const std::string& arg : args) {
    command += L" " + std::wstring(arg.begin(), arg.end());
  }
  SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES),
                              .lpSecurityDescriptor = nullptr,
                              .bInheritHandle = TRUE};
  wil::unique_handle read;
  wil::unique_handle write;
  EXPECT_TRUE(::CreatePipe(read.put(), write.put(), &inherit, 0));
  ::SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write.get();
  PROCESS_INFORMATION info{};
  if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &info) == FALSE) {
    ADD_FAILURE() << "CreateProcess failed: " << ::GetLastError();
    return {};
  }
  const wil::unique_handle process(info.hProcess);
  const wil::unique_handle thread(info.hThread);
  write.reset();
  Spawned spawned;
  std::string buffer(4096, '\0');
  DWORD bytes = 0;
  while (::ReadFile(read.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes,
                    nullptr) != FALSE &&
         bytes > 0) {
    spawned.out.append(buffer.data(), bytes);
  }
  EXPECT_EQ(::WaitForSingleObject(process.get(), 30'000), WAIT_OBJECT_0);
  ::GetExitCodeProcess(process.get(), &spawned.code);
  return spawned;
}

// Deploy contract C6, through the executable the deploy script runs.
TEST_F(CliTest, ExecutableStatusJsonFollowsTheDeployContract) {
  start();
  const Spawned healthy = spawn_cli({"--pipe", pipe_token(pipe()), "status", "--json"});
  EXPECT_EQ(healthy.code, static_cast<DWORD>(exit_ok));
  EXPECT_EQ(Json::parse(healthy.out).at("health"), "ok");
  EXPECT_EQ(healthy.out.find('\r'), std::string::npos);

  const Spawned down = spawn_cli({"--pipe", nobody(), "status", "--json"});
  EXPECT_EQ(down.code, static_cast<DWORD>(exit_unavailable));
  EXPECT_EQ(Json::parse(down.out).at("health"), "unreachable");
}

}  // namespace
}  // namespace myclean::cli
