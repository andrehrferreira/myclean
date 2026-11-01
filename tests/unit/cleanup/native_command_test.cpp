#include "core/cleanup/native_command.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "fakes.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::cleanup {
namespace {

using namespace myclean::literals;
using std::filesystem::path;
using testing::FakeRunner;

class NativeCommandTest : public ::testing::Test {
 protected:
  NativeCommand action(std::string command) {
    return NativeCommand{runner_, file_system_, std::move(command), Tier::automatic};
  }

  FakeRunner& runner() { return runner_; }
  fakes::FakeFileSystem& fs() { return file_system_; }

 private:
  FakeRunner runner_;
  fakes::FakeFileSystem file_system_;
};

TEST(NativeTools, OnlyTheAllowListedCommandsAreKnown) {
  for (const char* command :
       {"pnpm store prune", "npm cache verify", "yarn cache clean", "pip cache purge",
        "uv cache prune", "dotnet nuget locals all --clear", "go clean -cache -modcache",
        "docker system prune", "wsl tmp prune"}) {
    EXPECT_NE(find_native_tool(command), nullptr) << command;
  }
  EXPECT_NE(find_native_tool("  pnpm   store prune "), nullptr);
  EXPECT_EQ(find_native_tool("pnpm store prune --force"), nullptr);
  EXPECT_EQ(find_native_tool("npm cache clean --force"), nullptr);
  EXPECT_EQ(find_native_tool("cmd /c rd /s /q C:\\"), nullptr);
  EXPECT_EQ(find_native_tool("docker system prune")->prune, "system prune --force");
}

TEST_F(NativeCommandTest, PreviewMeasuresEveryFolderTheToolReports) {
  fs().add_file(R"(C:\Users\o\AppData\Local\go-build\00\a)", 3_MiB);
  fs().add_file(R"(C:\Users\o\go\pkg\mod\cache\b)", 5_MiB);
  runner().answer("go env GOCACHE GOMODCACHE",
                  "C:\\Users\\o\\AppData\\Local\\go-build\r\nC:\\Users\\o\\go\\pkg\\mod\r\n");
  NativeCommand prune = action("go clean -cache -modcache");

  const Result<Preview> preview = prune.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_EQ(preview->items[0].path, path("C:\\Users\\o\\AppData\\Local\\go-build"));
  EXPECT_EQ(preview->items[0].size, 3_MiB);
  EXPECT_EQ(preview->items[1].size, 5_MiB);
  EXPECT_EQ(runner().calls, std::vector<std::string>{"go env GOCACHE GOMODCACHE"});
  EXPECT_EQ(runner().timeouts.front(), NativeCommand::locate_timeout);
}

TEST_F(NativeCommandTest, LabeledOutputIsReadAndOtherLinesIgnored) {
  fs().add_file(R"(C:\Users\o\.nuget\packages\x\1.0\x.nupkg)", 2_MiB);
  runner().answer(
      "dotnet nuget locals all --list",
      "Welcome to .NET!\r\nhttp-cache: C:\\Users\\o\\AppData\\Local\\NuGet\\v3-cache\r\n"
      "global-packages: C:\\Users\\o\\.nuget\\packages\\\r\n"
      "temp: C:\\Users\\o\\AppData\\Local\\Temp\\NuGetScratch\r\n");
  NativeCommand prune = action("dotnet nuget locals all --clear");

  const Result<Preview> preview = prune.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 3U);
  EXPECT_EQ(preview->items[1].path, path("C:\\Users\\o\\.nuget\\packages\\"));
  EXPECT_EQ(preview->total_size(), 2_MiB);
}

TEST_F(NativeCommandTest, ApplyRunsThePruneCommandOnceAndVerifyMeasuresAgain) {
  fs().add_file(R"(C:\Users\o\AppData\Local\pnpm\store\v10\files\00\a)", 7_MiB);
  fs().add_file(R"(C:\Users\o\AppData\Local\pnpm\store\v10\files\00\b)", 1_MiB);
  runner().answer("pnpm store path", "C:\\Users\\o\\AppData\\Local\\pnpm\\store\\v10\n");
  runner().answer("pnpm store prune", "Removed 12 packages\n");
  runner().on_run = [&](const std::string& call) {
    if (call == "pnpm store prune") {  // what the tool itself does
      fs().erase(R"(C:\Users\o\AppData\Local\pnpm\store\v10\files\00\a)");
    }
  };
  NativeCommand prune = action("pnpm store prune");
  Result<Preview> preview = prune.preview();
  ASSERT_TRUE(preview);
  preview->items[0].id = PlanItemId{3};

  const Result<Undo> undo = prune.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(runner().calls, (std::vector<std::string>{"pnpm store path", "pnpm store prune"}));
  EXPECT_EQ(runner().timeouts.back(), NativeCommand::prune_timeout);
  EXPECT_TRUE(fs().operations().empty());  // MyClean deleted nothing itself
  EXPECT_EQ(undo->not_reversible, "regenerates");
  ASSERT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(undo->items[0].item, PlanItemId{3});

  const Result<Verification> verification = prune.verify(*undo);
  ASSERT_TRUE(verification) << verification.error().message;
  EXPECT_EQ(verification->metric, "cache_bytes");
  EXPECT_EQ(verification->before, static_cast<double>((8_MiB).count()));
  EXPECT_EQ(verification->after, static_cast<double>((1_MiB).count()));
  EXPECT_EQ(verification->effect, Effect::better);

  const Result<void> undone = prune.undo(*undo);
  ASSERT_FALSE(undone);
  EXPECT_EQ(undone.error().code, Errc::not_reversible);
}

TEST_F(NativeCommandTest, NonZeroExitFailsWithTheToolsFirstErrorLine) {
  fs().add_file(R"(C:\Users\o\AppData\Local\npm-cache\_cacache\x)", 1_MiB);
  runner().answer("npm config get cache", "C:\\Users\\o\\AppData\\Local\\npm-cache\n");
  runner().answer("npm cache verify", "", 1, "npm error code EPERM\nnpm error syscall open\n");
  NativeCommand prune = action("npm cache verify");
  const Result<Preview> preview = prune.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = prune.apply(*preview);

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::io_error);
  EXPECT_EQ(undo.error().message, "`npm cache verify` exited with code 1: npm error code EPERM");
}

TEST_F(NativeCommandTest, NeverRunsWithoutTheUserContext) {
  runner().user = false;  // the service, as LocalSystem
  NativeCommand prune = action("pnpm store prune");

  const Result<Preview> preview = prune.preview();
  const Result<Undo> undo =
      prune.apply(Preview{.items = {PlanItem{.path = "C:\\pnpm\\store", .size = 1_MiB}}});

  ASSERT_FALSE(preview);
  EXPECT_EQ(preview.error().code, Errc::refused);
  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::refused);
  EXPECT_NE(undo.error().message.find("user context is unavailable"), std::string::npos);
  EXPECT_TRUE(runner().calls.empty());
}

TEST_F(NativeCommandTest, CommandOutsideTheAllowListNeverRuns) {
  NativeCommand prune = action("cmd /c rd /s /q C:\\Users");

  const Result<Undo> undo =
      prune.apply(Preview{.items = {PlanItem{.path = "C:\\Users", .size = 1_MiB}}});

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::refused);
  EXPECT_TRUE(runner().calls.empty());
}

TEST_F(NativeCommandTest, ToolMissingFromPathIsNotFound) {
  NativeCommand prune = action("uv cache prune");

  const Result<Preview> preview = prune.preview();

  ASSERT_FALSE(preview);
  EXPECT_EQ(preview.error().code, Errc::not_found);
  EXPECT_NE(preview.error().message.find("`uv cache dir`"), std::string::npos);
}

TEST_F(NativeCommandTest, OutputWithoutAPathIsInvalid) {
  runner().answer("pip cache dir",
                  "ERROR: pip cache commands can not function since cache is disabled.\n");
  NativeCommand prune = action("pip cache purge");

  const Result<Preview> preview = prune.preview();

  ASSERT_FALSE(preview);
  EXPECT_EQ(preview.error().code, Errc::invalid_data);
}

TEST_F(NativeCommandTest, EmptyPreviewRunsNothing) {
  NativeCommand prune = action("yarn cache clean");

  const Result<Undo> undo = prune.apply(Preview{});

  ASSERT_TRUE(undo);
  EXPECT_TRUE(undo->items.empty());
  EXPECT_TRUE(runner().calls.empty());
}

TEST_F(NativeCommandTest, DockerReportsReclaimableBytesWithoutVolumes) {
  runner().answer("docker system df --format {{.Type}}={{.Reclaimable}}",
                  "Images=1.5GB (40%)\nContainers=0B (0%)\nLocal Volumes=2GB (100%)\n"
                  "Build Cache=512.5MB\n");
  runner().answer("docker system prune --force", "Total reclaimed space: 2GB\n");
  NativeCommand prune = action("docker system prune");

  const Result<Preview> preview = prune.preview();
  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_EQ(preview->items[0].path, path("docker:Images"));
  EXPECT_EQ(preview->items[0].size, Bytes{1'500'000'000});
  EXPECT_EQ(preview->items[1].path, path("docker:Build Cache"));
  EXPECT_EQ(preview->items[1].size, Bytes{512'500'000});

  ASSERT_TRUE(prune.apply(*preview));
  EXPECT_EQ(runner().calls.back(), "docker system prune --force");
}

// `text` as wsl.exe writes it: UTF-16LE, each ASCII character followed by a NUL byte.
std::string utf16(std::string_view text) {
  std::string wide;
  for (const char character : text) {
    wide += character;
    wide += '\0';
  }
  return wide;
}

constexpr const char* wsl_list = "wsl -l -q";
constexpr const char* ubuntu_du = "wsl -d Ubuntu-24.04 -u root --exec du -sk /tmp";
constexpr const char* debian_du = "wsl -d Debian -u root --exec du -sk /tmp";
constexpr const char* find_args =
    "-u root --exec find /tmp -mindepth 1 -maxdepth 1 -mtime +7 ! -name .*-unix -exec rm -rf {} +";

TEST_F(NativeCommandTest, WslListsDistributionsFromUtf16AndMeasuresTmpInEach) {
  runner().answer(wsl_list,
                  utf16("Ubuntu-24.04\r\ndocker-desktop\r\nDebian\r\ndocker-desktop-data\r\n"));
  runner().answer(ubuntu_du, "98566144\t/tmp\n");
  runner().answer(debian_du, "12\t/tmp\n");
  NativeCommand prune = action("wsl tmp prune");

  const Result<Preview> preview = prune.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_EQ(preview->items[0].path, path("wsl:Ubuntu-24.04:/tmp"));
  EXPECT_EQ(preview->items[0].size, Bytes{98566144ULL * 1024});
  EXPECT_EQ(preview->items[1].path, path("wsl:Debian:/tmp"));
  EXPECT_EQ(preview->items[1].size, Bytes{12ULL * 1024});
  EXPECT_EQ(runner().calls, (std::vector<std::string>{wsl_list, ubuntu_du, debian_du}));
  EXPECT_EQ(runner().arguments[1], (std::vector<std::string>{"-d", "Ubuntu-24.04", "-u", "root",
                                                             "--exec", "du", "-sk", "/tmp"}));
  EXPECT_EQ(runner().timeouts,
            (std::vector<std::chrono::milliseconds>{NativeCommand::locate_timeout,
                                                    NativeCommand::wsl_measure_timeout,
                                                    NativeCommand::wsl_measure_timeout}));
  EXPECT_TRUE(fs().operations().empty());  // nothing read or changed through the rootfs
}

TEST_F(NativeCommandTest, WslListToleratesAByteOrderMarkTheDefaultMarkAndUtf8) {
  // The mark as the runner's OEM fallback leaves it, a `wsl -l -v` style default mark, and a
  // name with a code unit beyond ASCII, which is skipped rather than guessed.
  runner().answer(wsl_list, "\xC2\xA0\xE2\x96\xA0" + utf16("* Debian\r\nUb") + "\xE9" +
                                std::string(1, '\0') + utf16("\r\n"));
  runner().answer(debian_du, "4\t/tmp\n");
  NativeCommand prune = action("wsl tmp prune");

  const Result<Preview> preview = prune.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, path("wsl:Debian:/tmp"));
  EXPECT_EQ(runner().calls, (std::vector<std::string>{wsl_list, debian_du}));

  FakeRunner utf8;  // with WSL_UTF8=1 wsl.exe writes UTF-8
  utf8.answer(wsl_list, "Debian\n");
  utf8.answer(debian_du, "4\t/tmp\n");
  NativeCommand plain{utf8, fs(), "wsl tmp prune", Tier::automatic};
  const Result<Preview> again = plain.preview();
  ASSERT_TRUE(again) << again.error().message;
  ASSERT_EQ(again->items.size(), 1U);
}

TEST_F(NativeCommandTest, WslDistributionWhoseMeasureFailsIsLeftOutUnlessAllFail) {
  runner().answer(wsl_list, utf16("Ubuntu-24.04\r\nDebian\r\n"));
  runner().answer(ubuntu_du, "", 1, "du: cannot read directory '/tmp': Permission denied\n");
  runner().answer(debian_du, "8\t/tmp\n");
  NativeCommand prune = action("wsl tmp prune");

  const Result<Preview> preview = prune.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, path("wsl:Debian:/tmp"));

  runner().answer(debian_du, "not a size\n");
  const Result<Preview> none = prune.preview();
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().code, Errc::io_error);
  EXPECT_NE(none.error().message.find("Permission denied"), std::string::npos);
}

TEST_F(NativeCommandTest, WslApplyRunsFindAsRootInEachPreviewedDistribution) {
  runner().answer(wsl_list, utf16("Ubuntu-24.04\r\nDebian\r\n"));
  runner().answer(ubuntu_du, "1000\t/tmp\n");
  runner().answer(debian_du, "10\t/tmp\n");
  const std::string ubuntu_find = std::string("wsl -d Ubuntu-24.04 ") + find_args;
  const std::string debian_find = std::string("wsl -d Debian ") + find_args;
  runner().answer(ubuntu_find, "");
  runner().answer(debian_find, "");
  runner().on_run = [&](const std::string& call) {
    if (call == ubuntu_find) {  // what find does inside Linux
      runner().answer(ubuntu_du, "100\t/tmp\n");
    }
  };
  NativeCommand prune = action("wsl tmp prune");
  Result<Preview> preview = prune.preview();
  ASSERT_TRUE(preview) << preview.error().message;
  preview->items.push_back(PlanItem{.path = R"(C:\Users\o\tmp)", .size = 1_MiB});  // not WSL's
  runner().calls.clear();
  runner().arguments.clear();
  runner().timeouts.clear();

  const Result<Undo> undo = prune.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(runner().calls, (std::vector<std::string>{ubuntu_find, debian_find}));
  EXPECT_EQ(runner().arguments[0],
            (std::vector<std::string>{
                "-d",    "Ubuntu-24.04", "-u",  "root",   "--exec", "find", "/tmp",  "-mindepth",
                "1",     "-maxdepth",    "1",   "-mtime", "+7",     "!",    "-name", ".*-unix",
                "-exec", "rm",           "-rf", "{}",     "+"}));
  EXPECT_EQ(runner().timeouts,
            (std::vector<std::chrono::milliseconds>{NativeCommand::wsl_prune_timeout,
                                                    NativeCommand::wsl_prune_timeout}));
  EXPECT_TRUE(fs().operations().empty());

  const Result<Verification> verification = prune.verify(*undo);
  ASSERT_TRUE(verification) << verification.error().message;
  EXPECT_EQ(verification->before, static_cast<double>((1010ULL * 1024) + (1_MiB).count()));
  EXPECT_EQ(verification->after, static_cast<double>(110ULL * 1024));
  EXPECT_EQ(verification->effect, Effect::better);
}

TEST_F(NativeCommandTest, WslPruneThatFailsInOneDistributionStillRunsInTheOthers) {
  const std::string ubuntu_find = std::string("wsl -d Ubuntu-24.04 ") + find_args;
  const std::string debian_find = std::string("wsl -d Debian ") + find_args;
  runner().answer(ubuntu_find, "", 1, "find: '/tmp/x': Operation not permitted\n");
  runner().answer(debian_find, "");
  NativeCommand prune = action("wsl tmp prune");

  const Result<Undo> undo =
      prune.apply(Preview{.items = {PlanItem{.path = "wsl:Ubuntu-24.04:/tmp", .size = 1_MiB},
                                    PlanItem{.path = "wsl:Debian:/tmp", .size = 1_MiB}}});

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::io_error);
  EXPECT_NE(undo.error().message.find("Operation not permitted"), std::string::npos);
  EXPECT_EQ(runner().calls, (std::vector<std::string>{ubuntu_find, debian_find}));
}

TEST_F(NativeCommandTest, DescribeRunsInTheUserContext) {
  const NativeCommand prune{runner(), fs(), "pnpm store prune", Tier::ask};

  const ActionInfo info = prune.describe();

  EXPECT_EQ(info.context, ExecutionContext::user);
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.what, "Run `pnpm store prune`");
}

}  // namespace
}  // namespace myclean::cleanup
