#include "core/placement/system_space.hpp"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "placement_fakes.hpp"

namespace myclean::placement {
namespace {

using namespace literals;
using testing::FakeDriveFacts;
using testing::FakeProcessRunner;
using testing::FakeSystemSpace;

// The owner's machine: 127.2 GB of memory, full hibernation with a 50.9 GB hiberfil.sys.
constexpr Bytes memory{136'592'523'264ULL};
constexpr Bytes full_file{54'636'994'560ULL};

class SystemSpaceTest : public ::testing::Test {
 protected:
  SystemSpaceTest() {
    system_.hibernation_state = HibernationState{.mode = HibernationMode::full,
                                                 .file = LR"(C:\hiberfil.sys)",
                                                 .file_size = full_file,
                                                 .memory = memory};
    system_.page_file_state = PageFileState{
        .config = {.automatic = true},
        .in_use = {PageFileUse{.path = LR"(C:\pagefile.sys)", .allocated = 89178_MiB}}};
    system_.shadows = {ShadowStorage{
        .volume = "C:", .diff_volume = "C:", .used = 3_GiB, .allocated = 4_GiB, .maximum = 95_GiB}};
    drives_.list() = testing::owner_volumes();
    // powercfg's effect on the hibernation settings.
    runner_.on_run = [this](const std::string& command) {
      HibernationState& state = system_.hibernation_state;
      if (command == "powercfg /h off") {
        state.mode = HibernationMode::off;
      } else if (command == "powercfg /h on" || command == "powercfg /h /type full") {
        state.mode = HibernationMode::full;
      } else if (command == "powercfg /h /type reduced") {
        state.mode = HibernationMode::reduced;
      }
      state.file_size = hibernation_size(state.memory, state.mode, state.size_percent);
    };
  }

  FakeSystemSpace& system() { return system_; }
  FakeProcessRunner& runner() { return runner_; }
  FakeDriveFacts& drives() { return drives_; }

 private:
  FakeSystemSpace system_;
  FakeProcessRunner runner_;
  FakeDriveFacts drives_;
};

TEST_F(SystemSpaceTest, HibernationEntryShowsCostRecommendationCommandAndUndo) {
  EXPECT_EQ(recommended_hibernation(system().hibernation_state), HibernationMode::reduced);
  const HibernationAction action{system(), runner(), HibernationMode::reduced};

  const Result<SettingSummary> summary = action.summary();

  ASSERT_TRUE(summary.has_value());
  EXPECT_EQ(summary->current, "full (hiberfil.sys 50.9 GB)");
  EXPECT_EQ(summary->proposed, "reduced (about 25.4 GB)");
  EXPECT_EQ(summary->cost, full_file);
  EXPECT_EQ(summary->commands, std::vector<std::string>{"powercfg /h /type reduced"});
  EXPECT_EQ(summary->undo_commands, std::vector<std::string>{"powercfg /h /type full"});
  EXPECT_FALSE(summary->restart_required);
  EXPECT_EQ(action.describe().tier, Tier::ask);
  EXPECT_EQ(action.describe().context, ExecutionContext::service);
  EXPECT_TRUE(runner().commands.empty());
}

TEST_F(SystemSpaceTest, HibernationApplyVerifyAndUndo) {
  HibernationAction action{system(), runner(), HibernationMode::reduced};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->items.at(0).path, LR"(C:\hiberfil.sys)");
  EXPECT_EQ(preview->total_size(), full_file);

  const Result<Undo> data = action.apply(*preview);
  ASSERT_TRUE(data.has_value()) << data.error().message;
  EXPECT_EQ(system().hibernation_state.mode, HibernationMode::reduced);
  const Result<Verification> verified = action.verify(*data);
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(verified->metric, "hiberfil_bytes");
  EXPECT_EQ(verified->effect, Effect::better);

  ASSERT_TRUE(action.undo(*data).has_value());
  EXPECT_EQ(system().hibernation_state.mode, HibernationMode::full);
  EXPECT_EQ(runner().commands,
            (std::vector<std::string>{"powercfg /h /type reduced", "powercfg /h /type full"}));
}

TEST_F(SystemSpaceTest, HibernationOffAndBackOnAgain) {
  HibernationAction action{system(), runner(), HibernationMode::off};
  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(system().hibernation_state.file_size, Bytes{});
  ASSERT_TRUE(action.undo(*data).has_value());
  EXPECT_EQ(system().hibernation_state.mode, HibernationMode::full);
  EXPECT_EQ(runner().commands, (std::vector<std::string>{"powercfg /h off", "powercfg /h on",
                                                         "powercfg /h /type full"}));
}

TEST_F(SystemSpaceTest, HibernationCommandsRestoreACustomFullSize) {
  const HibernationState reduced{.mode = HibernationMode::reduced, .size_percent = 0};
  std::vector<std::string> commands;
  for (const Command& command : hibernation_commands(reduced, HibernationMode::full, 60)) {
    commands.push_back(command.text());
  }
  EXPECT_EQ(commands, (std::vector<std::string>{"powercfg /h /type full", "powercfg /h /size 60"}));
  EXPECT_TRUE(hibernation_commands(reduced, HibernationMode::reduced, 0).empty());
}

TEST_F(SystemSpaceTest, HibernationUndoRefusesWhenTheModeChangedMeanwhile) {
  HibernationAction action{system(), runner(), HibernationMode::reduced};
  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_TRUE(data.has_value());
  system().hibernation_state.mode = HibernationMode::off;
  const Result<void> undone = action.undo(*data);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::invalid_state);
  EXPECT_EQ(runner().commands.size(), 1U);
}

TEST_F(SystemSpaceTest, AFailedPowercfgReversesWhatAlreadyRan) {
  system().hibernation_state.mode = HibernationMode::off;
  runner().exit_codes["powercfg /h /type reduced"] = 1;
  HibernationAction action{system(), runner(), HibernationMode::reduced};

  const Result<Undo> data = action.apply(*action.preview());

  ASSERT_FALSE(data.has_value());
  EXPECT_NE(data.error().message.find("Access is denied."), std::string::npos);
  EXPECT_EQ(system().hibernation_state.mode, HibernationMode::off);
  EXPECT_EQ(
      runner().commands,
      (std::vector<std::string>{"powercfg /h on", "powercfg /h /type reduced", "powercfg /h off"}));
}

TEST_F(SystemSpaceTest, HibernationAlreadyInTheModeIsNotChanged) {
  HibernationAction action{system(), runner(), HibernationMode::full};
  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_FALSE(data.has_value());
  EXPECT_EQ(data.error().code, Errc::invalid_state);
  EXPECT_EQ(recommended_hibernation({.mode = HibernationMode::reduced}), std::nullopt);
}

TEST_F(SystemSpaceTest, PageFileRecommendationKeepsASmallFileOnCAndMovesTheRestToTheBestNvme) {
  const std::optional<PageFileConfig> recommended =
      recommended_page_files(system().page_file_state, drives().list(), L"C:\\");
  ASSERT_TRUE(recommended.has_value());
  EXPECT_EQ(
      *recommended,
      (PageFileConfig{
          .files = {PageFile{.path = LR"(C:\pagefile.sys)", .initial_mb = 4096, .maximum_mb = 4096},
                    PageFile{.path = LR"(D:\pagefile.sys)"}}}));
  PageFileState capped = system().page_file_state;
  capped.config = *recommended;
  EXPECT_EQ(recommended_page_files(capped, drives().list(), L"C:\\"), std::nullopt);
}

TEST_F(SystemSpaceTest, PageFileMoveAndUndoRestoreSystemManagedSizingAndNeedARestart) {
  const PageFileConfig moved{.files = {PageFile{.path = LR"(D:\pagefile.sys)"}}};
  PageFileAction action{system(), drives(), moved};

  const Result<SettingSummary> summary = action.summary();
  ASSERT_TRUE(summary.has_value());
  EXPECT_TRUE(summary->restart_required);
  EXPECT_EQ(summary->current, "managed by Windows");
  EXPECT_EQ(summary->proposed, R"(D:\pagefile.sys system-managed size)");
  EXPECT_EQ(summary->cost, 89178_MiB);
  EXPECT_EQ(
      summary->commands,
      (std::vector<std::string>{
          "set Win32_ComputerSystem AutomaticManagedPagefile=False",
          R"(set Win32_PageFileSetting Name='D:\pagefile.sys' InitialSize=0 MaximumSize=0)"}));
  EXPECT_EQ(summary->undo_commands,
            (std::vector<std::string>{R"(delete Win32_PageFileSetting Name='D:\pagefile.sys')",
                                      "set Win32_ComputerSystem AutomaticManagedPagefile=True"}));

  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_TRUE(data.has_value()) << data.error().message;
  EXPECT_EQ(system().page_file_state.config, moved);
  EXPECT_EQ(action.verify(*data)->effect, Effect::better);

  ASSERT_TRUE(action.undo(*data).has_value());
  EXPECT_EQ(system().page_file_state.config, PageFileConfig{.automatic = true});
}

TEST_F(SystemSpaceTest, PageFileOnAnUnhealthyDriveIsRefused) {
  drives().at(L"D:\\").healthy = false;
  PageFileAction action{system(), drives(), {.files = {PageFile{.path = LR"(D:\pagefile.sys)"}}}};
  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_FALSE(data.has_value());
  EXPECT_EQ(data.error().code, Errc::refused);
  EXPECT_TRUE(system().written.empty());
}

TEST_F(SystemSpaceTest, PageFileUndoRefusesWhenTheConfigurationChangedMeanwhile) {
  PageFileAction action{system(), drives(), {.files = {PageFile{.path = LR"(D:\pagefile.sys)"}}}};
  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_TRUE(data.has_value());
  system().page_file_state.config.files[0].maximum_mb = 1024;
  const Result<void> undone = action.undo(*data);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::invalid_state);
  EXPECT_EQ(system().written.size(), 1U);
}

TEST_F(SystemSpaceTest, ShadowStorageShowsTheVssadminCommandAndUndoesToThePreviousLimit) {
  const std::optional<Bytes> recommended = recommended_shadow_maximum(system().shadows[0], 952_GiB);
  ASSERT_TRUE(recommended.has_value());
  EXPECT_EQ(*recommended, Bytes{952ULL * (1ULL << 30U) / 100 * 5});
  runner().on_run = [this](const std::string& command) {
    system().shadows[0].maximum =
        command.ends_with("UNBOUNDED")
            ? std::nullopt
            : std::optional<Bytes>{Bytes{std::stoull(command.substr(command.rfind('=') + 1))}};
  };
  ShadowStorageAction action{system(), runner(), "C:", 10_GiB};

  const Result<SettingSummary> summary = action.summary();
  ASSERT_TRUE(summary.has_value());
  EXPECT_EQ(summary->commands,
            std::vector<std::string>{
                "vssadmin resize shadowstorage /for=C: /on=C: /maxsize=10737418240"});
  EXPECT_EQ(summary->undo_commands,
            std::vector<std::string>{
                "vssadmin resize shadowstorage /for=C: /on=C: /maxsize=102005473280"});
  EXPECT_EQ(summary->cost, 4_GiB);
  EXPECT_EQ(action.preview()->items.at(0).path, LR"(C:\System Volume Information)");

  const Result<Undo> data = action.apply(*action.preview());
  ASSERT_TRUE(data.has_value()) << data.error().message;
  EXPECT_EQ(system().shadows[0].maximum, std::optional<Bytes>{10_GiB});
  EXPECT_EQ(action.verify(*data)->effect, Effect::better);
  ASSERT_TRUE(action.undo(*data).has_value());
  EXPECT_EQ(system().shadows[0].maximum, std::optional<Bytes>{95_GiB});
}

TEST_F(SystemSpaceTest, ShadowStorageUnboundedAndTheVssadminMinimum) {
  system().shadows[0].maximum = std::nullopt;
  EXPECT_EQ(shadow_storage_command(system().shadows[0], std::nullopt).text(),
            "vssadmin resize shadowstorage /for=C: /on=C: /maxsize=UNBOUNDED");
  ShadowStorageAction tiny{system(), runner(), "C:", 100_MiB};
  const Result<Undo> data = tiny.apply(*tiny.preview());
  ASSERT_FALSE(data.has_value());
  EXPECT_EQ(data.error().code, Errc::invalid_argument);
  ShadowStorageAction missing{system(), runner(), "Q:", 10_GiB};
  EXPECT_EQ(missing.preview().error().code, Errc::not_found);
  EXPECT_TRUE(runner().commands.empty());
}

}  // namespace
}  // namespace myclean::placement
