#include "core/user_tasks/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../clean/clean_fakes.hpp"
#include "../placement/placement_fakes.hpp"
#include "core/model/action.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/safety/deny_list.hpp"
#include "core/user_tasks/task.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::user_tasks {
namespace {

namespace fs = std::filesystem;
using namespace literals;
using Json = nlohmann::json;

const fs::path folder = LR"(C:\Users\owner\AppData\Local\OldStudio)";
const fs::path archive = LR"(L:\Archive\C\Users\owner\AppData\Local\OldStudio)";

safety::ProtectedLocations owner_locations() {
  return {.windows = LR"(C:\Windows)",
          .profiles = LR"(C:\Users)",
          .owner_profile = LR"(C:\Users\owner)",
          .owner_known_folders = {LR"(C:\Users\owner\Documents)", LR"(C:\Users\owner\Desktop)"},
          .myclean = {LR"(C:\ProgramData\MyClean)"}};
}

class ArchiveTest : public ::testing::Test {
 protected:
  ArchiveTest() {
    drives_.list() = placement::testing::owner_volumes();
    for (const placement::TargetVolume& volume : drives_.list()) {
      fs_.add_directory(volume.root);
    }
    fs_.add_file(folder / "scenes" / "a.scene", 3_MiB);
    fs_.add_file(folder / "scenes" / "b.scene", 2_MiB);
    fs_.add_file(folder / "settings.ini", 1_MiB);
  }

  ArchiveAction action(fs::path source = folder, fs::path target = archive,
                       std::optional<FileIdentity> expected = std::nullopt) {
    return ArchiveAction(tools(), PlanId{30},
                         Archive{.source = std::move(source), .target = std::move(target)},
                         expected);
  }
  ArchiveTools tools() {
    return ArchiveTools{.placement = &placement_, .usage = &usage_, .locations = &locations_};
  }

  // Previews and applies `archiving`; the undo data, or the failure.
  static Result<Undo> run(ArchiveAction& archiving) {
    Result<Preview> preview = archiving.preview();
    if (!preview) {
      return tl::unexpected(preview.error());
    }
    preview->items.front().id = PlanItemId{301};
    return archiving.apply(*preview);
  }

  [[nodiscard]] bool holds_folder() const {
    return fs_.exists(folder / "scenes" / "a.scene") && fs_.exists(folder / "scenes" / "b.scene") &&
           fs_.exists(folder / "settings.ini");
  }
  [[nodiscard]] bool holds_archive() const {
    return fs_.exists(archive / "scenes" / "a.scene") &&
           fs_.exists(archive / "scenes" / "b.scene") && fs_.exists(archive / "settings.ini");
  }

  fakes::FakeFileSystem& fs() { return fs_; }
  placement::testing::FakeTreeCopier& copier() { return copier_; }
  placement::testing::FakeDriveFacts& drives() { return drives_; }
  clean::testing::FakeUsage& usage() { return usage_; }
  [[nodiscard]] const placement::Tools& placement() const { return placement_; }
  [[nodiscard]] const safety::ProtectedLocations& locations() const { return locations_; }

 private:
  fakes::FakeFileSystem fs_;
  placement::testing::FakeTreeCopier copier_{fs_};
  placement::testing::FakeDriveFacts drives_;
  clean::testing::FakeUsage usage_;
  safety::ProtectedLocations locations_ = owner_locations();
  placement::Tools placement_{.copier = &copier_, .file_system = &fs_, .drives = &drives_};
};

// Owner-decisions spec, "Move to HDD archive": copied, checked, then removed; undo brings it back.
TEST_F(ArchiveTest, CopiesChecksThenRemovesTheFolderAndUndoBringsItBack) {
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);

  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_TRUE(undo->reversible());
  EXPECT_FALSE(fs().exists(folder));
  EXPECT_TRUE(holds_archive());
  ASSERT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(undo->items[0].item, PlanItemId{301});
  const Json record = Json::parse(undo->items[0].data);
  EXPECT_EQ(record.at("files"), 3);
  EXPECT_EQ(record.at("bytes"), (6_MiB).count());

  const Result<Verification> verified = archiving.verify(*undo);
  ASSERT_TRUE(verified.has_value()) << verified.error().message;
  EXPECT_EQ(verified->metric, "source_bytes");
  EXPECT_EQ(verified->before, static_cast<double>((6_MiB).count()));
  EXPECT_EQ(verified->after, 0.0);
  EXPECT_EQ(verified->effect, Effect::better);

  const Result<void> undone = archiving.undo(*undo);
  ASSERT_TRUE(undone.has_value()) << undone.error().message;
  EXPECT_TRUE(holds_folder());
  EXPECT_FALSE(fs().exists(archive));
}

TEST_F(ArchiveTest, AFolderARunningProgramUsesIsRefusedBeforeAnyCopy) {
  usage().add_process_folder(folder / "scenes");
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::in_use);
  EXPECT_EQ(copier().copies(), 0);
  EXPECT_TRUE(holds_folder());
}

TEST_F(ArchiveTest, SystemAndDenyListPlacesAreRefusedBeforeAnyCopy) {
  fs().add_file(LR"(C:\Program Files\Tool\tool.exe)", 1_MiB);
  fs().add_file(LR"(C:\Users\owner\Documents\Old\a.doc)", 1_MiB);
  fs().add_file(LR"(C:\Users\other\Old\a.doc)", 1_MiB);
  for (const fs::path& source :
       {fs::path(LR"(C:\Program Files\Tool)"), fs::path(LR"(C:\Users\owner\Documents\Old)"),
        fs::path(LR"(C:\Users\other\Old)")}) {
    ArchiveAction archiving = action(source, LR"(L:\Archive\C)" / source.relative_path());
    const Result<Undo> undo = run(archiving);
    ASSERT_FALSE(undo.has_value()) << source;
    EXPECT_EQ(undo.error().code, Errc::refused) << undo.error().message;
  }
  EXPECT_EQ(copier().copies(), 0);
}

TEST_F(ArchiveTest, ATargetThatIsNotTheArchiveFolderOrLacksRoomIsRefused) {
  ArchiveAction elsewhere = action(folder, LR"(L:\Elsewhere)");
  EXPECT_EQ(run(elsewhere).error().code, Errc::refused);
  ArchiveAction same_volume =
      action(folder, LR"(C:\Archive\C\Users\owner\AppData\Local\OldStudio)");
  EXPECT_EQ(run(same_volume).error().code, Errc::refused);
  drives().at(LR"(L:\)").free = Bytes{drives().at(LR"(L:\)").size.count() / 20};  // < 10 % free
  ArchiveAction full = action();
  EXPECT_EQ(run(full).error().code, Errc::refused);
  drives().at(LR"(L:\)").free = drives().at(LR"(L:\)").size;
  drives().at(LR"(L:\)").healthy = false;
  ArchiveAction failing = action();
  EXPECT_EQ(run(failing).error().code, Errc::refused);
  EXPECT_EQ(copier().copies(), 0);
  EXPECT_TRUE(holds_folder());
}

TEST_F(ArchiveTest, AFolderReplacedSinceThePlanWasMadeIsRefused) {
  ArchiveAction archiving = action(folder, archive, FileIdentity{.volume_serial = 99});
  const Result<Undo> undo = run(archiving);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::refused);
  EXPECT_EQ(copier().copies(), 0);
}

// Owner-decisions spec: nothing is deleted before a verified copy exists.
TEST_F(ArchiveTest, ACopyThatDoesNotMatchRemovesNothingAndLeavesNoArchiveBehind) {
  copier().skip_one_file();
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::io_error);
  EXPECT_NE(undo.error().message.find("nothing was removed"), std::string::npos);
  EXPECT_TRUE(holds_folder());
  EXPECT_FALSE(fs().exists(archive));
}

TEST_F(ArchiveTest, AFailedCopyRemovesNothing) {
  copier().fail_with(Errc::access_denied);
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::access_denied);
  EXPECT_TRUE(holds_folder());
}

TEST_F(ArchiveTest, AnArchiveFolderThatIsNotEmptyIsNeverOverwritten) {
  fs().add_file(archive / "other.bin", 1_MiB);
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::not_empty);
  EXPECT_TRUE(holds_folder());
  EXPECT_TRUE(fs().exists(archive / "other.bin"));
}

TEST_F(ArchiveTest, AnEntryTheSafetyLayerKeepsLeavesTheArchiveComplete) {
  fs().fail_on(folder / "settings.ini", Errc::access_denied,
               fakes::FakeFileSystem::OperationKind::remove);
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_TRUE(holds_archive());
  EXPECT_TRUE(fs().exists(folder / "settings.ini"));
  EXPECT_FALSE(fs().exists(folder / "scenes"));
  const Result<Verification> verified = archiving.verify(*undo);
  ASSERT_TRUE(verified.has_value()) << verified.error().message;
  EXPECT_EQ(verified->after, static_cast<double>((1_MiB).count()));
  EXPECT_EQ(verified->effect, Effect::better);
  // The folder holds something else now: undo copies nothing over it.
  EXPECT_EQ(archiving.undo(*undo).error().code, Errc::not_empty);
  EXPECT_TRUE(holds_archive());
}

TEST_F(ArchiveTest, UndoRefusesWhenTheArchiveCopyChangedOrTheVolumeLacksRoom) {
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;

  drives().at(LR"(C:\)").free = Bytes{1024};
  EXPECT_EQ(archiving.undo(*undo).error().code, Errc::refused);
  EXPECT_FALSE(fs().exists(folder / "settings.ini"));

  drives().at(LR"(C:\)").free = drives().at(LR"(C:\)").size;
  fs().add_file(archive / "new.txt", 1_KiB);
  EXPECT_EQ(archiving.undo(*undo).error().code, Errc::invalid_state);
  EXPECT_FALSE(archiving.verify(*undo).has_value());
  EXPECT_TRUE(fs().exists(archive / "new.txt"));
}

TEST_F(ArchiveTest, AnUndoWhoseCopyIsBackAlreadyOnlyRemovesTheArchiveCopy) {
  ArchiveAction archiving = action();
  const Result<Undo> undo = run(archiving);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  ASSERT_TRUE(copier().copy_tree(archive, folder).has_value());  // a first undo got this far
  const int copies = copier().copies();

  ASSERT_TRUE(archiving.undo(*undo).has_value());
  EXPECT_EQ(copier().copies(), copies);
  EXPECT_TRUE(holds_folder());
  EXPECT_FALSE(fs().exists(archive));
}

TEST(ArchiveIdentityTest, ReadsAndWritesTheFileIdOfATask) {
  FileIdentity identity{.volume_serial = 7};
  identity.file_id.at(0) = 0xAB;
  identity.file_id.at(15) = 0x01;
  const std::string text = file_id_text(identity);
  EXPECT_EQ(text, "ab000000000000000000000000000001");
  EXPECT_EQ(parse_identity(7, text), identity);
  EXPECT_EQ(parse_identity(7, "AB000000000000000000000000000001"), identity);
  EXPECT_EQ(parse_identity(7, ""), std::nullopt);
  EXPECT_EQ(parse_identity(7, "zz000000000000000000000000000001"), std::nullopt);
}

// The tray reads an archive task of user_tasks.next and runs it.
TEST_F(ArchiveTest, TheTrayRunsAnArchiveTaskAndReportsItsUndo) {
  const FileIdentity identity = fs().stat(folder).value().identity;
  const Json task{{"plan", 30},
                  {"title", "Move the folder to the HDD archive"},
                  {"kind", "archive"},
                  {"operation", "apply"},
                  {"tier", "ask"},
                  {"action", {{"source", folder.string()}, {"target", archive.string()}}},
                  {"items", Json::array({Json{{"id", 301},
                                              {"path", folder.string()},
                                              {"bytes", (6_MiB).count()},
                                              {"approved", true},
                                              {"volume_serial", identity.volume_serial},
                                              {"file_id", file_id_text(identity)}}})}};
  const Result<UserTask> parsed = parse_task(task);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->kind, TaskKind::archive);
  EXPECT_EQ(parsed->archive.target, archive);
  EXPECT_EQ(parsed->items.front().identity, identity);

  const Toolbox missing{.placement = placement()};
  EXPECT_EQ(user_tasks::run(*parsed, missing, {}).state, PlanState::failed);
  EXPECT_TRUE(holds_folder());

  const Toolbox toolbox{.placement = placement(), .usage = &usage(), .locations = &locations()};
  const TaskReport report = user_tasks::run(*parsed, toolbox, {});
  EXPECT_EQ(report.state, PlanState::verified) << report.error;
  EXPECT_TRUE(holds_archive());
  ASSERT_EQ(report.undo.items.size(), 1U);
  EXPECT_EQ(report.undo.items[0].item, PlanItemId{301});
}

}  // namespace
}  // namespace myclean::user_tasks
