#include "core/placement/relocation.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "fakes/fake_file_system.hpp"
#include "placement_fakes.hpp"

namespace myclean::placement {
namespace {

using namespace literals;
using fakes::FakeFileSystem;
using testing::FakeDriveFacts;
using testing::FakeTextFiles;
using testing::FakeTreeCopier;
using testing::FakeUserEnvironment;

constexpr std::string_view default_temp = R"(%USERPROFILE%\AppData\Local\Temp)";
constexpr std::string_view owner_path =
    R"(C:\Users\owner\.cargo\bin;%USERPROFILE%\go\bin;C:\Tools)";

class RelocationTest : public ::testing::Test {
 protected:
  RelocationTest() {
    drives_.list() = testing::owner_volumes();
    for (const TargetVolume& volume : drives_.list()) {
      fs_.add_directory(volume.root);
    }
    fs_.add_directory(LR"(C:\Users\owner)");
    env_.put("TEMP", std::string(default_temp), true);
    env_.put("TMP", std::string(default_temp), true);
    env_.put("Path", std::string(owner_path), true);
  }

  // Previews and applies `relocation`, expecting success.
  Undo relocate(const Relocation& relocation) {
    RelocateAction action{tools_, relocation};
    const Result<Preview> preview = action.preview();
    EXPECT_TRUE(preview.has_value()) << preview.error().message;
    Result<Undo> undo = action.apply(*preview);
    EXPECT_TRUE(undo.has_value()) << undo.error().message;
    return *undo;
  }

  Result<Undo> try_relocate(const Relocation& relocation) {
    RelocateAction action{tools_, relocation};
    const Result<Preview> preview = action.preview();
    if (!preview) {
      return tl::unexpected(preview.error());
    }
    return action.apply(*preview);
  }

  Result<void> undo(const Undo& data, CacheKind kind) {
    RelocateAction action{tools_, Relocation{.kind = kind}};  // as rebuilt after a restart
    return action.undo(data);
  }

  [[nodiscard]] std::optional<std::string> text(std::string_view name) const {
    const std::optional<EnvValue> value = env_.value(name);
    return value ? std::optional<std::string>{value->text} : std::nullopt;
  }

  FakeUserEnvironment& env() { return env_; }
  FakeTextFiles& files() { return files_; }
  FakeFileSystem& fs() { return fs_; }
  FakeTreeCopier& copier() { return copier_; }
  FakeDriveFacts& drives() { return drives_; }
  const Tools& tools() { return tools_; }

 private:
  FakeUserEnvironment env_;
  FakeTextFiles files_;
  FakeFileSystem fs_;
  FakeTreeCopier copier_{fs_};
  FakeDriveFacts drives_;
  Tools tools_{.environment = &env_,
               .files = &files_,
               .copier = &copier_,
               .file_system = &fs_,
               .drives = &drives_};
};

TEST_F(RelocationTest, PreviewShowsTheLocationSizeAndATargetOffTheSystemDriveAndChangesNothing) {
  fs().add_file(LR"(C:\Users\owner\AppData\Local\npm-cache\_cacache\index-v5\a)", 1400_MiB);
  env().put("GRADLE_USER_HOME", R"(F:\gradle)");

  const Result<std::vector<RelocationProposal>> rows = propose_relocations(tools());

  ASSERT_TRUE(rows.has_value()) << rows.error().message;
  ASSERT_EQ(rows->size(), all_cache_kinds.size());
  const auto row = [&](CacheKind kind) {
    return *std::ranges::find_if(
        *rows, [&](const RelocationProposal& proposal) { return proposal.current.kind == kind; });
  };
  const RelocationProposal npm = row(CacheKind::npm);
  EXPECT_EQ(npm.current.path, LR"(C:\Users\owner\AppData\Local\npm-cache)");
  EXPECT_EQ(npm.current.size, 1400_MiB);
  EXPECT_EQ(npm.current.setting, "npm config cache");
  EXPECT_FALSE(npm.current.configured);
  ASSERT_TRUE(npm.target.has_value());
  EXPECT_EQ(*npm.target, LR"(D:\DevCaches\npm-cache)");
  EXPECT_EQ(npm.content, ContentMode::regenerate);
  EXPECT_EQ(row(CacheKind::temp).target, std::optional<std::filesystem::path>{LR"(D:\Temp)"});
  EXPECT_EQ(row(CacheKind::cargo).content, ContentMode::move);
  EXPECT_EQ(row(CacheKind::gradle).target, std::nullopt);
  EXPECT_EQ(row(CacheKind::gradle).reason, "already off the system drive");
  EXPECT_EQ(row(CacheKind::go_modules).current.path, LR"(C:\Users\owner\go\pkg\mod)");
  EXPECT_EQ(row(CacheKind::unreal_ddc).current.setting, "UE-LocalDataCachePath");

  EXPECT_EQ(env().writes(), 0);
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(RelocationTest, GoModulesDefaultFollowsGopath) {
  env().put("GOPATH", R"(%USERPROFILE%\work\go;C:\other)", true);
  const Result<CacheLocation> location = locate(tools(), CacheKind::go_modules);
  ASSERT_TRUE(location.has_value());
  EXPECT_EQ(location->path, LR"(C:\Users\owner\work\go\pkg\mod)");
}

TEST_F(RelocationTest, TempMovesThroughBothUserVariablesWithoutAJunction) {
  const Undo data = relocate({.kind = CacheKind::temp, .target = LR"(D:\Temp)"});

  EXPECT_EQ(env().value("TEMP"), (EnvValue{.text = R"(D:\Temp)", .expandable = true}));
  EXPECT_EQ(text("TMP"), std::optional<std::string>{R"(D:\Temp)"});
  EXPECT_EQ(env().announcements(), 1);
  // The only file system change is the new folder: no junction, nothing at the old path.
  ASSERT_EQ(fs().operations().size(), 1U);
  EXPECT_EQ(fs().operations()[0].kind, FakeFileSystem::OperationKind::create_directories);
  EXPECT_EQ(fs().operations()[0].from, LR"(D:\Temp)");

  ASSERT_TRUE(undo(data, CacheKind::temp).has_value());
  EXPECT_EQ(env().value("TEMP"), (EnvValue{.text = std::string(default_temp), .expandable = true}));
  EXPECT_EQ(text("TMP"), std::optional<std::string>{default_temp});
  EXPECT_EQ(env().announcements(), 2);
}

TEST_F(RelocationTest, CargoKeepsItsToolsOnPathAndUndoRestoresPathExactly) {
  env().put("Path", R"(%USERPROFILE%\.cargo\bin\;C:\Tools)", true);
  fs().add_file(LR"(C:\Users\owner\.cargo\bin\cargo.exe)", 10_MiB);

  const Undo data =
      relocate({.kind = CacheKind::cargo, .target = LR"(W:\cargo)", .content = ContentMode::move});

  EXPECT_EQ(text("CARGO_HOME"), std::optional<std::string>{R"(W:\cargo)"});
  EXPECT_EQ(env().value("Path"),
            (EnvValue{.text = R"(W:\cargo\bin;C:\Tools)", .expandable = true}));
  EXPECT_TRUE(fs().exists(LR"(W:\cargo\bin\cargo.exe)"));

  ASSERT_TRUE(undo(data, CacheKind::cargo).has_value());
  EXPECT_EQ(env().value("Path"),
            (EnvValue{.text = R"(%USERPROFILE%\.cargo\bin\;C:\Tools)", .expandable = true}));
  EXPECT_EQ(env().value("CARGO_HOME"), std::nullopt);
}

TEST_F(RelocationTest, CargoCannotRegenerateBecauseItsToolsLiveInItsBinFolder) {
  const Result<Undo> result = try_relocate({.kind = CacheKind::cargo, .target = LR"(D:\cargo)"});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::refused);
  EXPECT_EQ(env().writes(), 0);
}

TEST_F(RelocationTest, RegenerateLeavesTheOldContentAsASeparateCleaningItem) {
  fs().add_file(LR"(C:\Users\owner\.gradle\caches\modules-2\a.jar)", 4_GiB);

  const Undo data = relocate({.kind = CacheKind::gradle, .target = LR"(D:\DevCaches\gradle)"});

  EXPECT_EQ(text("GRADLE_USER_HOME"), std::optional<std::string>{R"(D:\DevCaches\gradle)"});
  EXPECT_TRUE(fs().exists(LR"(C:\Users\owner\.gradle\caches\modules-2\a.jar)"));
  EXPECT_EQ(copier().copies(), 0);

  const Result<Plan> leftover =
      leftover_plan(fs(), data, Actor::owner(), WallTime{std::chrono::seconds{1}});
  ASSERT_TRUE(leftover.has_value()) << leftover.error().message;
  EXPECT_EQ(leftover->action, "QuarantineItems");
  ASSERT_EQ(leftover->items.size(), 1U);
  EXPECT_EQ(leftover->items[0].path, LR"(C:\Users\owner\.gradle)");
  EXPECT_EQ(leftover->items[0].size, 4_GiB);
  EXPECT_EQ(leftover->items[0].identity, fs().stat(LR"(C:\Users\owner\.gradle)")->identity);
}

TEST_F(RelocationTest, MovedContentIsAtTheTargetBeforeTheSettingSwitches) {
  fs().add_file(LR"(C:\Users\owner\.nuget\packages\newtonsoft.json\13.0.3\a.nupkg)", 2_MiB);
  bool present_at_switch = false;
  env().on_write([&](std::string_view name) {
    if (name == "NUGET_PACKAGES") {
      present_at_switch =
          fs().exists(LR"(D:\DevCaches\nuget-packages\newtonsoft.json\13.0.3\a.nupkg)");
    }
  });

  relocate({.kind = CacheKind::nuget,
            .target = LR"(D:\DevCaches\nuget-packages)",
            .content = ContentMode::move});

  EXPECT_TRUE(present_at_switch);
  EXPECT_EQ(text("NUGET_PACKAGES"), std::optional<std::string>{R"(D:\DevCaches\nuget-packages)"});
}

TEST_F(RelocationTest, AnIncompleteCopyChangesNoSetting) {
  fs().add_file(LR"(C:\Users\owner\.nuget\packages\a\1\a.nupkg)", 1_MiB);
  fs().add_file(LR"(C:\Users\owner\.nuget\packages\b\1\b.nupkg)", 1_MiB);
  copier().skip_one_file();

  const Result<Undo> result = try_relocate(
      {.kind = CacheKind::nuget, .target = LR"(D:\nuget)", .content = ContentMode::move});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::io_error);
  EXPECT_EQ(env().writes(), 0);
}

TEST_F(RelocationTest, MovingIntoAFolderThatHoldsFilesIsRefused) {
  fs().add_file(LR"(C:\Users\owner\.nuget\packages\a\1\a.nupkg)", 1_MiB);
  fs().add_file(LR"(D:\nuget\other.txt)", 1_KiB);
  const Result<Undo> result = try_relocate(
      {.kind = CacheKind::nuget, .target = LR"(D:\nuget)", .content = ContentMode::move});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::not_empty);
  EXPECT_EQ(copier().copies(), 0);
}

TEST_F(RelocationTest, UndoDeletesAVariableThatWasNotSet) {
  const Undo data = relocate({.kind = CacheKind::pip, .target = LR"(D:\DevCaches\pip-cache)"});
  EXPECT_EQ(text("PIP_CACHE_DIR"), std::optional<std::string>{R"(D:\DevCaches\pip-cache)"});

  ASSERT_TRUE(undo(data, CacheKind::pip).has_value());
  EXPECT_EQ(env().value("PIP_CACHE_DIR"), std::nullopt);
}

TEST_F(RelocationTest, NpmCacheIsSetInTheUserNpmrcAndUndoRemovesWhatItAdded) {
  const std::filesystem::path npmrc = LR"(C:\Users\owner\.npmrc)";
  const Undo created = relocate({.kind = CacheKind::npm, .target = LR"(D:\DevCaches\npm-cache)"});
  EXPECT_EQ(*files().read(npmrc), std::optional<std::string>{"cache=D:\\DevCaches\\npm-cache\n"});
  EXPECT_EQ(locate(tools(), CacheKind::npm)->path, LR"(D:\DevCaches\npm-cache)");
  ASSERT_TRUE(undo(created, CacheKind::npm).has_value());
  EXPECT_FALSE(files().exists(npmrc));  // the file did not exist before

  ASSERT_TRUE(files().write(npmrc, "registry=https://registry.npmjs.org/\r\n").has_value());
  const Undo edited = relocate({.kind = CacheKind::npm, .target = LR"(D:\DevCaches\npm-cache)"});
  ASSERT_TRUE(undo(edited, CacheKind::npm).has_value());
  EXPECT_EQ(*files().read(npmrc),
            std::optional<std::string>{"registry=https://registry.npmjs.org/\r\n"});
}

TEST_F(RelocationTest, NpmConfigCacheVariableWinsOverTheNpmrc) {
  env().put("npm_config_cache", R"(C:\npm)");
  relocate({.kind = CacheKind::npm, .target = LR"(D:\npm)"});
  EXPECT_EQ(text("npm_config_cache"), std::optional<std::string>{R"(D:\npm)"});
  EXPECT_FALSE(files().exists(LR"(C:\Users\owner\.npmrc)"));
}

TEST_F(RelocationTest, ATargetOnADriveThatDegradedSinceTheProposalIsRefusedAtApply) {
  RelocateAction action{tools(), {.kind = CacheKind::go_build, .target = LR"(G:\go-build)"}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  drives().at(L"G:\\").healthy = false;
  drives().at(L"G:\\").health_note = "disk 3 health is warning; 1 new media error";

  const Result<Undo> result = action.apply(*preview);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::refused);
  EXPECT_NE(result.error().message.find("media error"), std::string::npos);
  EXPECT_EQ(env().writes(), 0);
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(RelocationTest, ALocationChangedSinceThePreviewIsRefused) {
  RelocateAction action{tools(), {.kind = CacheKind::gradle, .target = LR"(D:\gradle)"}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  env().put("GRADLE_USER_HOME", R"(F:\gradle)");
  const Result<Undo> result = action.apply(*preview);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::refused);
}

TEST_F(RelocationTest, ATargetInsideTheCurrentFolderIsRejected) {
  const Result<Undo> result =
      try_relocate({.kind = CacheKind::temp, .target = LR"(C:\Users\owner\AppData\Local\Temp\x)"});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::invalid_argument);
}

TEST_F(RelocationTest, AFailedWriteRestoresTheSettingsAlreadyWritten) {
  env().fail_on("TMP");
  const Result<Undo> result = try_relocate({.kind = CacheKind::temp, .target = LR"(D:\Temp)"});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::access_denied);
  EXPECT_EQ(env().value("TEMP"), (EnvValue{.text = std::string(default_temp), .expandable = true}));
}

TEST_F(RelocationTest, UndoRefusesWithoutChangesWhenASettingWasChangedMeanwhile) {
  const Undo data = relocate({.kind = CacheKind::temp, .target = LR"(D:\Temp)"});
  env().put("TEMP", R"(E:\Elsewhere)");

  const Result<void> result = undo(data, CacheKind::temp);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::invalid_state);
  EXPECT_EQ(text("TMP"), std::optional<std::string>{R"(D:\Temp)"});
}

TEST_F(RelocationTest, VerifyCountsTheSettingsThatHoldTheNewValue) {
  RelocateAction action{tools(), {.kind = CacheKind::temp, .target = LR"(D:\Temp)"}};
  const Undo data = *action.apply(*action.preview());
  Result<Verification> verified = action.verify(data);
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(verified->metric, "relocated_settings");
  EXPECT_EQ(verified->effect, Effect::better);

  env().put("TMP", R"(C:\Temp)");
  verified = action.verify(data);
  EXPECT_EQ(verified->after, 1.0);
  EXPECT_EQ(verified->effect, Effect::worse);
}

TEST_F(RelocationTest, DescribesAnApprovalOnlyChangeInTheOwnersContext) {
  const ActionInfo info =
      RelocateAction{tools(), {.kind = CacheKind::npm, .target = LR"(D:\DevCaches\npm-cache)"}}
          .describe();
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.context, ExecutionContext::user);
  EXPECT_NE(info.what.find(R"(D:\DevCaches\npm-cache)"), std::string::npos);
}

TEST_F(RelocationTest, MalformedUndoDataIsRejected) {
  const Undo data{.items = {UndoItem{.data = R"({"kind":"temp"})"}}};
  const Result<void> result = undo(data, CacheKind::temp);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::invalid_data);
}

}  // namespace
}  // namespace myclean::placement
