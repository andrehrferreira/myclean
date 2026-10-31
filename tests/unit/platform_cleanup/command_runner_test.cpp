#include "core/cleanup/command_runner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "platform/cleanup/win32_command_runner.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using cleanup::Command;
using cleanup::CommandOutput;
using std::chrono::milliseconds;
using std::chrono::seconds;

// A fresh folder of harmless scripts, removed after each test.
class CommandRunnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    sandbox_ = fs::temp_directory_path() /
               ("myclean-runner-" + std::to_string(::GetCurrentProcessId()) + "-" + info->name());
    fs::create_directories(sandbox_);
    write("report-tool.cmd", "@echo off\r\necho [%1] [%2]\r\necho problem 1>&2\r\nexit /b 3\r\n");
    write("slow-tool.cmd", "@echo off\r\nping -n 30 127.0.0.1 >nul\r\n");
    write("ask-tool.cmd", "@echo off\r\nset /p answer=Continue? \r\necho got[%answer%]\r\n");
  }

  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(sandbox_, ignored);
  }

  void write(const std::string& name, std::string_view text) const {
    std::ofstream(sandbox_ / name, std::ios::binary) << text;
  }

  [[nodiscard]] Win32CommandRunner runner() const { return Win32CommandRunner{{sandbox_}}; }
  [[nodiscard]] const fs::path& sandbox() const { return sandbox_; }

 private:
  fs::path sandbox_;
};

TEST_F(CommandRunnerTest, RunsAScriptFoundOnTheSearchPath) {
  Win32CommandRunner tools = runner();

  const Result<CommandOutput> output =
      tools.run(Command{.program = "report-tool", .arguments = {"cache", "dir"}}, {});

  ASSERT_TRUE(output) << output.error().message;
  EXPECT_EQ(output->exit_code, 3U);
  EXPECT_EQ(output->out, "[cache] [dir]\r\n");
  EXPECT_EQ(output->err, "problem \r\n");
}

TEST_F(CommandRunnerTest, RunsAnAbsoluteProgram) {
  Win32CommandRunner tools = runner();

  const Result<CommandOutput> output =
      tools.run(Command{.program = (sandbox() / "report-tool.cmd").string()}, {});

  ASSERT_TRUE(output) << output.error().message;
  EXPECT_EQ(output->out, "[] []\r\n");
}

TEST_F(CommandRunnerTest, InputIsEndOfFile) {
  Win32CommandRunner tools = runner();

  const Result<CommandOutput> output =
      tools.run(Command{.program = "ask-tool", .timeout = seconds(30)}, {});

  ASSERT_TRUE(output) << output.error().message;
  EXPECT_NE(output->out.find("got[]"), std::string::npos);
}

TEST_F(CommandRunnerTest, TimeoutEndsTheProcessTree) {
  Win32CommandRunner tools = runner();
  const auto started = std::chrono::steady_clock::now();

  const Result<CommandOutput> output =
      tools.run(Command{.program = "slow-tool", .timeout = milliseconds(300)}, {});

  ASSERT_FALSE(output);
  EXPECT_EQ(output.error().code, Errc::io_error);
  EXPECT_NE(output.error().message.find("timed out"), std::string::npos);
  // ping holds the output pipe; the run only ends this soon when the job ended it too.
  EXPECT_LT(std::chrono::steady_clock::now() - started, seconds(10));
}

TEST_F(CommandRunnerTest, StopRequestCancels) {
  Win32CommandRunner tools = runner();
  std::stop_source source;
  source.request_stop();

  const Result<CommandOutput> output =
      tools.run(Command{.program = "slow-tool", .timeout = seconds(60)}, source.get_token());

  ASSERT_FALSE(output);
  EXPECT_EQ(output.error().code, Errc::refused);
}

TEST_F(CommandRunnerTest, ScriptArgumentWithACmdMetacharacterIsRefused) {
  Win32CommandRunner tools = runner();

  const Result<CommandOutput> output =
      tools.run(Command{.program = "report-tool", .arguments = {"a&calc"}}, {});

  ASSERT_FALSE(output);
  EXPECT_EQ(output.error().code, Errc::invalid_argument);
}

TEST_F(CommandRunnerTest, UnknownProgramIsNotFound) {
  const Result<fs::path> found = runner().resolve("myclean-no-such-tool");

  ASSERT_FALSE(found);
  EXPECT_EQ(found.error().code, Errc::not_found);
}

TEST_F(CommandRunnerTest, RelativeSearchFoldersAreIgnored) {
  const Win32CommandRunner tools{{fs::path("relative")}};

  EXPECT_FALSE(tools.resolve("report-tool"));
}

TEST_F(CommandRunnerTest, ProgramWithAFolderIsRefused) {
  const Result<fs::path> found = runner().resolve("sub\\report-tool");

  ASSERT_FALSE(found);
  EXPECT_EQ(found.error().code, Errc::invalid_argument);
}

TEST(CommandRunner, ResolvesAndRunsAnExecutable) {
  const fs::path system = dism_path().parent_path();
  Win32CommandRunner tools{{system}};

  const Result<fs::path> found = tools.resolve("whoami");
  ASSERT_TRUE(found) << found.error().message;
  EXPECT_EQ(*found, system / "whoami.exe");
  EXPECT_EQ(tools.resolve("WHOAMI.EXE").value_or(fs::path{}), system / "WHOAMI.EXE");

  const Result<CommandOutput> output = tools.run(Command{.program = "whoami"}, {});
  ASSERT_TRUE(output) << output.error().message;
  EXPECT_EQ(output->exit_code, 0U);
  EXPECT_FALSE(output->out.empty());
}

TEST(CommandRunner, TestsRunInAUserContext) {
  EXPECT_TRUE(Win32CommandRunner{}.user_context());
}

TEST(CommandRunner, DismIsInTheSystemFolder) {
  EXPECT_TRUE(fs::is_regular_file(dism_path()));
}

}  // namespace
}  // namespace myclean::platform
