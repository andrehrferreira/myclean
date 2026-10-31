#include "core/cleanup/native_command.hpp"

#include <filesystem>
#include <string>
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
        "docker system prune"}) {
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

TEST_F(NativeCommandTest, DescribeRunsInTheUserContext) {
  const NativeCommand prune{runner(), fs(), "pnpm store prune", Tier::ask};

  const ActionInfo info = prune.describe();

  EXPECT_EQ(info.context, ExecutionContext::user);
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.what, "Run `pnpm store prune`");
}

}  // namespace
}  // namespace myclean::cleanup
