#include "svc/host.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ipc/security.hpp"
#include "platform/service/system.hpp"
#include "svc/component.hpp"
#include "svc_test_support.hpp"

namespace myclean::svc {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using testing::TempDir;

TEST(CommandLineTest, NoArgumentsMeansService) {
  const Result<HostOptions> options = parse_command_line({});
  ASSERT_TRUE(options.has_value());
  EXPECT_EQ(options->mode, Mode::service);
  EXPECT_FALSE(options->state_dir.has_value());
  EXPECT_EQ(options->pipe_name, platform::default_pipe_name);
}

TEST(CommandLineTest, ConsoleTakesStateAndPipe) {
  const std::vector<std::wstring> args{L"--console", L"--state", LR"(C:\dev\state)", L"--pipe",
                                       L"myclean-dev"};
  const Result<HostOptions> options = parse_command_line(args);
  ASSERT_TRUE(options.has_value()) << options.error().message;
  EXPECT_EQ(options->mode, Mode::console);
  EXPECT_EQ(options->state_dir, fs::path(LR"(C:\dev\state)"));
  EXPECT_EQ(options->pipe_name, LR"(\\.\pipe\myclean-dev)");

  const std::vector<std::wstring> relative{L"--console", L"--state", L"state"};
  const Result<HostOptions> made_absolute = parse_command_line(relative);
  ASSERT_TRUE(made_absolute.has_value());
  EXPECT_TRUE(made_absolute->state_dir.value_or(fs::path()).is_absolute());

  const std::vector<std::wstring> bare{L"--console"};
  const Result<HostOptions> defaults = parse_command_line(bare);
  ASSERT_TRUE(defaults.has_value());
  EXPECT_FALSE(defaults->state_dir.has_value());
  EXPECT_EQ(defaults->pipe_name, platform::default_pipe_name);
}

TEST(CommandLineTest, RejectsAnythingElse) {
  const std::vector<std::vector<std::wstring>> invalid{
      {L"--service"},
      {L"--state", LR"(C:\x)"},
      {L"--console", L"--state"},
      {L"--console", L"--pipe", LR"(..\other)"},
      {L"--console", L"--pipe", L"a b"},
      {L"--console", L"--pipe", L"caf\u00e9"},
      {L"--console", L"--verbose", L"1"},
  };
  for (const std::vector<std::wstring>& args : invalid) {
    const Result<HostOptions> options = parse_command_line(args);
    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error().code, Errc::invalid_argument);
  }
}

TEST(InstallLayoutTest, DataAndManifestSitBesideBin) {
  const InstallLayout layout =
      install_layout(LR"(C:\Program Files\MyClean\versions\v1\bin\myclean-svc.exe)");
  EXPECT_EQ(layout.data_dir, fs::path(LR"(C:\Program Files\MyClean\versions\v1\data)"));
  EXPECT_EQ(layout.manifest, fs::path(LR"(C:\Program Files\MyClean\versions\v1\manifest.json)"));
}

TEST(ManifestTest, ReadsTheVersion) {
  const TempDir dir;
  const fs::path manifest = dir.path() / "manifest.json";
  // deploy.ps1 writes UTF-8; tolerate a byte order mark.
  std::ofstream(manifest, std::ios::binary)
      << "\xEF\xBB\xBF{\"version\": \"20261101-142310-3fa9c2d1\", \"hash\": \"abc\"}";
  EXPECT_EQ(manifest_version(manifest), "20261101-142310-3fa9c2d1");
}

TEST(ManifestTest, NoVersionWithoutAUsableManifest) {
  const TempDir dir;
  EXPECT_EQ(manifest_version(dir.path() / "missing.json"), std::nullopt);
  const fs::path broken = dir.path() / "broken.json";
  std::ofstream(broken) << "{version";
  EXPECT_EQ(manifest_version(broken), std::nullopt);
  const fs::path unversioned = dir.path() / "unversioned.json";
  std::ofstream(unversioned) << R"({"version": 3})";
  EXPECT_EQ(manifest_version(unversioned), std::nullopt);
}

class OwnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(file()) << "S-1-5-21-1000000000-2000000000-3000000000-1001\r\n";
    const Result<std::string> file_owner = platform::file_owner_sid(file());
    ASSERT_TRUE(file_owner.has_value());
    file_owner_ = *file_owner;
  }

  [[nodiscard]] fs::path file() const { return dir_.path() / "owner.sid"; }

  [[nodiscard]] const TempDir& dir() const { return dir_; }
  [[nodiscard]] const std::string& file_owner() const { return file_owner_; }

 private:
  TempDir dir_;
  std::string file_owner_;
};

TEST_F(OwnerTest, ReadsARecordedOwnerFromATrustedFile) {
  const std::array<std::string_view, 1> trusted{file_owner()};
  const Result<std::string> owner = resolve_owner(dir().path(), trusted);
  ASSERT_TRUE(owner.has_value()) << owner.error().message;
  EXPECT_EQ(*owner, "S-1-5-21-1000000000-2000000000-3000000000-1001");
}

TEST_F(OwnerTest, RefusesAFileThatAnotherAccountOwns) {
  const std::array<std::string_view, 1> trusted{platform::system_sid};
  const Result<std::string> owner = resolve_owner(dir().path(), trusted);
  ASSERT_FALSE(owner.has_value());
  EXPECT_EQ(owner.error().code, Errc::refused);
}

TEST_F(OwnerTest, RefusesAFileWithoutASid) {
  std::ofstream(file()) << "nobody";
  const std::array<std::string_view, 1> trusted{file_owner()};
  const Result<std::string> owner = resolve_owner(dir().path(), trusted);
  ASSERT_FALSE(owner.has_value());
  EXPECT_EQ(owner.error().code, Errc::invalid_data);
}

TEST(ResolveOwnerTest, WithoutARecordOnlyLocalSystemCanAskTheConsole) {
  const Result<std::string> user = platform::current_user_sid();
  ASSERT_TRUE(user.has_value());
  if (*user == platform::system_sid) {
    GTEST_SKIP() << "runs as LocalSystem";
  }
  const TempDir dir;
  const std::array<std::string_view, 1> trusted{platform::system_sid};
  const Result<std::string> owner = resolve_owner(dir.path(), trusted);
  ASSERT_FALSE(owner.has_value());
  EXPECT_FALSE(fs::exists(dir.path() / "owner.sid"));
}

// run() in console mode, in-process, on a thread; the stop event plays Ctrl+C.
class RunTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(SUCCEEDED(stop_.create(wil::EventOptions::ManualReset)));
    ASSERT_TRUE(SUCCEEDED(running_.create(wil::EventOptions::ManualReset)));
    options_.mode = Mode::console;
    options_.state_dir = dir_.path() / "state";
    options_.pipe_name = pipe_;
  }

  void TearDown() override {
    if (runner_.joinable()) {
      stop_.SetEvent();
      runner_.join();
    }
    // run() installed a logger that holds the log file open; release it before the folder goes.
    spdlog::drop_all();
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::null_sink_mt>()));
  }

  void start_run() {
    runner_ = std::thread(
        [this] { exit_code_ = run(options_, {}, stop_.get(), [this] { running_.SetEvent(); }); });
  }

  [[nodiscard]] std::string log_text() const {
    std::ifstream log(dir_.path() / "state" / "logs" / "myclean-svc.log");
    return {std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
  }

  [[nodiscard]] const TempDir& dir() const { return dir_; }
  [[nodiscard]] const std::wstring& pipe() const { return pipe_; }
  [[nodiscard]] wil::unique_event_nothrow& stop() { return stop_; }
  [[nodiscard]] wil::unique_event_nothrow& running() { return running_; }
  [[nodiscard]] std::thread& runner() { return runner_; }
  [[nodiscard]] int exit_code() const { return exit_code_; }

 private:
  TempDir dir_;
  std::wstring pipe_ = testing::unique_pipe_name();
  HostOptions options_;
  wil::unique_event_nothrow stop_;
  wil::unique_event_nothrow running_;
  std::thread runner_;
  int exit_code_ = -1;
};

TEST_F(RunTest, ConsoleModeServesUntilStopped) {
  start_run();
  ASSERT_TRUE(running().wait(30'000));
  {
    Result<platform::PipeClient> client = platform::PipeClient::connect(pipe(), 5s);
    ASSERT_TRUE(client.has_value()) << client.error().message;
    const Result<ipc::StatusReply> status = client->status();
    ASSERT_TRUE(status.has_value()) << status.error().message;
    EXPECT_TRUE(status->healthy);
    EXPECT_EQ(status->mode, "console");
    EXPECT_TRUE(status->components.empty());
  }
  stop().SetEvent();
  runner().join();
  EXPECT_EQ(exit_code(), exit_ok);
  EXPECT_TRUE(fs::exists(dir().path() / "state" / "myclean.db"));
  const std::string log = log_text();
  EXPECT_NE(log.find("integrity check passed"), std::string::npos) << log;
  EXPECT_NE(log.find("store flushed and closed"), std::string::npos) << log;
}

TEST_F(RunTest, StopsInsteadOfRunningWithoutThePipe) {
  const Result<std::string> user = platform::current_user_sid();
  ASSERT_TRUE(user.has_value());
  const Result<std::unique_ptr<platform::PipeServer>> squatter = platform::PipeServer::start(
      platform::PipeServerOptions{.name = pipe(),
                                  .owner_sid = *user,
                                  .handler = [](std::string_view) { return std::string(); },
                                  .on_error = {}});
  ASSERT_TRUE(squatter.has_value());

  start_run();
  runner().join();
  EXPECT_EQ(exit_code(), exit_start_failed);
  EXPECT_FALSE(running().is_signaled());
  const std::string log = log_text();
  EXPECT_NE(log.find("start failed (in_use)"), std::string::npos) << log;
}

}  // namespace
}  // namespace myclean::svc
