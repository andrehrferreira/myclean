#include "core/safety/safe_remove.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/plan_source.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::safety {
namespace {

using namespace myclean::literals;
using std::filesystem::path;

constexpr os::FileAttributes reparse{.reparse_point = true};

class StoredPlans final : public IPlanSource {
 public:
  StoredPlans() = default;

  Plan& add(PlanId id) {
    return plans_
        .insert_or_assign(
            id.value(),
            Plan{.id = id, .proposer = Actor::scheduler(), .state = PlanState::applying})
        .first->second;
  }

  [[nodiscard]] Result<Plan> load(PlanId id) const override {
    const auto found = plans_.find(id.value());
    if (found == plans_.end()) {
      return fail(Errc::not_found, "no plan " + std::to_string(id.value()));
    }
    return found->second;
  }

 private:
  std::map<std::int64_t, Plan> plans_;
};

ProtectedLocations owner_locations() {
  return {
      .windows = L"C:\\Windows",
      .profiles = L"C:\\Users",
      .owner_profile = L"C:\\Users\\owner",
      .owner_known_folders = {L"C:\\Users\\owner\\Documents", L"C:\\Users\\owner\\Desktop",
                              L"C:\\Users\\owner\\Pictures", L"C:\\Users\\owner\\Videos",
                              L"C:\\Users\\owner\\Music"},
      .myclean = {L"C:\\ProgramData\\MyClean", L"C:\\Program Files\\MyClean"},
  };
}

class SafeRemoveTest : public ::testing::Test {
 protected:
  static constexpr PlanId plan_id{42};

  // Adds `item_path` to the stored plan with the identity the entry has now (or `identity`).
  PlanItemId add_item(const path& item_path, std::optional<FileIdentity> identity = std::nullopt) {
    if (!identity) {
      const Result<os::FileStat> stat = fs_.stat(item_path);
      identity = stat ? stat->identity : FileIdentity{};
    }
    const PlanItemId id{static_cast<std::int64_t>(plan_.items.size()) + 1};
    plan_.items.push_back(PlanItem{.id = id, .path = item_path, .identity = *identity});
    return id;
  }

  std::vector<ItemOutcome> remove(const std::vector<PlanItemId>& items) {
    return safe_remove().remove(plan_id, items);
  }

  ItemOutcome remove_one(const path& item_path) { return remove({add_item(item_path)}).front(); }

  std::vector<ItemOutcome> move(const std::vector<MoveRequest>& requests) {
    return safe_remove().move(plan_id, requests);
  }

  SafeRemove safe_remove() { return SafeRemove::create(fs_, plans_, owner_locations()).value(); }

  // True when a recorded file system change names `folder` or anything inside it.
  [[nodiscard]] bool touched(const path& folder) const {
    const std::wstring prefix = folder.native() + L"\\";
    return std::ranges::any_of(fs_.operations(), [&](const fakes::FakeFileSystem::Operation& op) {
      return std::ranges::any_of(std::vector<path>{op.from, op.to}, [&](const path& named) {
        return named == folder || named.native().starts_with(prefix);
      });
    });
  }

  static void expect_refused(const ItemOutcome& outcome, std::string_view reason) {
    EXPECT_EQ(outcome.disposition, Disposition::refused) << to_utf8(outcome.path);
    EXPECT_EQ(outcome.reason, reason) << to_utf8(outcome.path);
    EXPECT_EQ(outcome.freed, Bytes{});
  }

  fakes::FakeFileSystem& fs() { return fs_; }
  Plan& plan() { return plan_; }

 private:
  fakes::FakeFileSystem fs_;
  StoredPlans plans_;
  Plan& plan_ = plans_.add(plan_id);
};

// One test per deny-list entry; each name starts with the entry's name in CamelCase, which
// SafeRemoveDenyListCoverage checks.
class SafeRemoveDenyList : public SafeRemoveTest {};

TEST_F(SafeRemoveDenyList, VolumeRootRefused) {
  fs().add_file(L"D:\\data.bin", 1_KiB);
  for (const path& spelling :
       {path{L"D:\\"}, path{L"d:/"}, path{L"\\\\?\\D:\\"}, path{L"D:\\x\\.."}}) {
    expect_refused(remove_one(spelling), "deny_list:volume_root");
  }
  EXPECT_TRUE(fs().operations().empty());
  EXPECT_TRUE(fs().exists(L"D:\\data.bin"));
}

TEST_F(SafeRemoveDenyList, WindirRefusedExceptAllowListedTemp) {
  fs().add_file(L"C:\\Windows\\System32\\drivers\\etc\\hosts", 1_KiB);
  fs().add_file(L"C:\\Windows\\Temp\\setup.log", 2_KiB);
  const std::vector<ItemOutcome> outcomes = remove({
      add_item(L"C:\\Windows\\System32\\drivers\\etc\\hosts"),
      add_item(L"C:\\Windows\\Temp\\setup.log"),
      add_item(L"C:\\Windows\\Temp"),
      add_item(L"C:\\Windows"),
  });
  expect_refused(outcomes[0], "deny_list:windir");
  EXPECT_EQ(outcomes[1].disposition, Disposition::removed);
  EXPECT_EQ(outcomes[1].freed, 2_KiB);
  expect_refused(outcomes[2], "deny_list:windir");  // the allow-listed folder itself stays
  expect_refused(outcomes[3], "deny_list:windir");
  EXPECT_TRUE(fs().exists(L"C:\\Windows\\System32\\drivers\\etc\\hosts"));
  EXPECT_FALSE(fs().exists(L"C:\\Windows\\Temp\\setup.log"));
}

TEST_F(SafeRemoveDenyList, ProgramFilesRefusedOnEveryVolume) {
  const std::vector<path> items = {
      L"C:\\Program Files\\App\\app.exe",
      L"C:\\Program Files (x86)\\App\\app.exe",
      L"C:\\Program Files\\Vendor\\cache",  // what a rule file matching it would plan
      L"D:\\Program Files\\Game\\game.exe",
  };
  std::vector<PlanItemId> ids;
  for (const path& item : items) {
    if (item.has_extension()) {
      fs().add_file(item, 1_KiB);
    } else {
      fs().add_file(item / L"blob.bin", 1_KiB);
    }
    ids.push_back(add_item(item));
  }
  for (const ItemOutcome& outcome : remove(ids)) {
    expect_refused(outcome, "deny_list:program_files");
  }
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveDenyList, GitFolderAndContentRefused) {
  fs().add_file(L"F:\\repo\\.git\\objects\\pack\\pack-1.pack", 1_MiB);
  fs().add_file(L"F:\\repo\\.git\\config", 1_KiB);
  const std::vector<ItemOutcome> outcomes = remove({
      add_item(L"F:\\repo\\.git\\objects\\pack\\pack-1.pack"),
      add_item(L"F:\\repo\\.git"),
      add_item(L"F:\\repo\\.GIT\\config"),
  });
  for (const ItemOutcome& outcome : outcomes) {
    expect_refused(outcome, "deny_list:git");
  }
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveDenyList, UserKnownFolderRefused) {
  const std::vector<path> items = {
      L"C:\\Users\\owner\\Documents\\thesis.docx", L"C:\\Users\\owner\\Desktop\\todo.txt",
      L"C:\\Users\\owner\\Pictures\\cat.jpg",      L"C:\\Users\\owner\\Videos\\clip.mp4",
      L"C:\\Users\\owner\\Music\\song.mp3",
  };
  std::vector<PlanItemId> ids;
  for (const path& item : items) {
    fs().add_file(item, 1_KiB);
    ids.push_back(add_item(item));
  }
  ids.push_back(add_item(L"C:\\Users\\owner\\Documents"));
  ids.push_back(add_item(L"C:\\Users\\owner"));  // contains the known folders
  for (const ItemOutcome& outcome : remove(ids)) {
    expect_refused(outcome, "deny_list:user_known_folder");
  }
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveDenyList, OtherProfileRefused) {
  fs().add_file(L"C:\\Users\\OtherUser\\AppData\\Local\\Temp\\a.tmp", 1_KiB);
  fs().add_file(L"C:\\Users\\Public\\Downloads\\setup.exe", 1_KiB);
  fs().add_file(L"C:\\Users\\owner2\\AppData\\Local\\Temp\\b.tmp", 1_KiB);
  fs().add_file(L"C:\\Users\\owner\\AppData\\Local\\Temp\\mine.tmp", 1_KiB);
  const std::vector<ItemOutcome> outcomes = remove({
      add_item(L"C:\\Users\\OtherUser\\AppData\\Local\\Temp"),
      add_item(L"C:\\Users\\Public\\Downloads\\setup.exe"),
      add_item(L"C:\\Users\\owner2\\AppData\\Local\\Temp\\b.tmp"),
      add_item(L"C:\\Users\\Default"),
      add_item(L"C:\\Users\\owner\\AppData\\Local\\Temp\\mine.tmp"),
  });
  for (std::size_t index = 0; index < 4; ++index) {
    expect_refused(outcomes[index], "deny_list:other_profile");
  }
  EXPECT_EQ(outcomes[4].disposition, Disposition::removed);
  EXPECT_FALSE(touched(L"C:\\Users\\OtherUser"));
}

TEST_F(SafeRemoveDenyList, MycleanDataAndInstallRefused) {
  fs().add_file(L"C:\\ProgramData\\MyClean\\myclean.db", 1_MiB);
  fs().add_file(L"C:\\Program Files\\MyClean\\current\\bin\\myclean-svc.exe", 1_MiB);
  const std::vector<ItemOutcome> outcomes = remove({
      add_item(L"C:\\ProgramData\\MyClean\\myclean.db"),
      add_item(L"C:\\ProgramData"),  // contains the data folder
      add_item(L"C:\\Program Files\\MyClean\\current\\bin\\myclean-svc.exe"),
  });
  expect_refused(outcomes[0], "deny_list:myclean");
  expect_refused(outcomes[1], "deny_list:myclean");
  EXPECT_EQ(outcomes[2].disposition, Disposition::refused);
  EXPECT_TRUE(outcomes[2].reason.starts_with("deny_list:"));
  EXPECT_TRUE(fs().operations().empty());
}

TEST(SafeRemoveDenyListCoverage, EveryEntryHasATest) {
  const auto camel_case = [](std::string_view name) {
    std::string text;
    bool upper = true;
    for (const char character : name) {
      if (character == '_') {
        upper = true;
        continue;
      }
      text += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
                    : character;
      upper = false;
    }
    return text;
  };
  const ::testing::UnitTest& unit = *::testing::UnitTest::GetInstance();
  for (const DenyRule& rule : deny_list()) {
    const std::string prefix = camel_case(rule.name);
    bool found = false;
    for (int suite_index = 0; suite_index < unit.total_test_suite_count(); ++suite_index) {
      const ::testing::TestSuite& suite = *unit.GetTestSuite(suite_index);
      if (std::string_view{suite.name()} != "SafeRemoveDenyList") {
        continue;
      }
      for (int test_index = 0; test_index < suite.total_test_count(); ++test_index) {
        found =
            found || std::string_view{suite.GetTestInfo(test_index)->name()}.starts_with(prefix);
      }
    }
    EXPECT_TRUE(found) << "deny-list entry '" << rule.name << "' has no test SafeRemoveDenyList."
                       << prefix << "*";
  }
}

TEST_F(SafeRemoveTest, TraversalSpellingRefused) {
  fs().add_file(L"C:\\Users\\owner\\Documents\\a.docx", 1_KiB);
  fs().add_directory(L"C:\\Users\\owner\\AppData\\Local\\Temp");
  expect_refused(
      remove_one(L"C:\\Users\\owner\\AppData\\Local\\Temp\\..\\..\\..\\Documents\\a.docx"),
      "deny_list:user_known_folder");
  EXPECT_TRUE(fs().exists(L"C:\\Users\\owner\\Documents\\a.docx"));
}

TEST_F(SafeRemoveTest, ShortNameSpellingRefused) {
  const FileIdentity app = fs().add_file(L"C:\\Program Files\\App\\app.exe", 1_KiB);
  fs().set_short_name(L"C:\\Program Files", L"PROGRA~1");
  const FileIdentity thesis = fs().add_file(L"C:\\Users\\owner\\Documents\\thesis.docx", 1_KiB);
  fs().set_short_name(L"C:\\Users\\owner\\Documents", L"DOCUME~1");
  const std::vector<ItemOutcome> outcomes = remove({
      add_item(L"C:\\PROGRA~1\\App\\app.exe", app),
      add_item(L"C:\\Users\\owner\\DOCUME~1\\thesis.docx", thesis),
  });
  expect_refused(outcomes[0], "deny_list:program_files");
  expect_refused(outcomes[1], "deny_list:user_known_folder");
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, OtherSpellingsOfADeniedPathRefused) {
  const FileIdentity kernel = fs().add_file(L"C:\\Windows\\System32\\kernel32.dll", 1_MiB);
  for (const path& spelling : {
           path{L"c:\\WINDOWS\\system32\\KERNEL32.DLL"},
           path{L"C:/Windows/System32/kernel32.dll"},
           path{L"\\\\?\\C:\\Windows\\System32\\kernel32.dll"},
           path{L"C:\\Windows.\\System32 \\kernel32.dll"},
           path{L"C:\\Windows\\\\Temp\\..\\System32\\kernel32.dll"},
       }) {
    expect_refused(remove({add_item(spelling, kernel)}).front(), "deny_list:windir");
  }
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, PathsThatAreNotAbsoluteDrivePathsRefused) {
  for (const path& spelling : {
           path{L"Temp\\a.tmp"},
           path{L"\\\\server\\share\\a.tmp"},
           path{L"\\\\.\\C:\\Temp\\a.tmp"},
           path{L"C:\\Temp\\a.tmp:stream"},
           path{L"C:\\Temp\\*.tmp"},
       }) {
    expect_refused(remove_one(spelling), "invalid_path");
  }
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, JunctionInsideFolderRemovedAsLinkOnly) {
  fs().add_file(L"F:\\store\\pkg\\index.js", 3_KiB);
  fs().add_file(L"F:\\proj\\node_modules\\real\\a.js", 1_KiB);
  fs().add_directory(L"F:\\proj\\node_modules\\pkg", reparse);
  const ItemOutcome outcome = remove_one(L"F:\\proj\\node_modules");
  EXPECT_EQ(outcome.disposition, Disposition::removed);
  EXPECT_EQ(outcome.freed, 1_KiB);
  EXPECT_FALSE(fs().exists(L"F:\\proj\\node_modules"));
  EXPECT_TRUE(fs().exists(L"F:\\store\\pkg\\index.js"));
  EXPECT_FALSE(touched(L"F:\\store"));
}

TEST_F(SafeRemoveTest, DirectorySymlinkToSystem32RemovedAsLinkOnly) {
  fs().add_file(L"C:\\Windows\\System32\\kernel32.dll", 1_MiB);
  fs().add_file(L"F:\\cache\\entry.bin", 1_KiB);
  fs().add_directory(L"F:\\cache\\system", reparse);
  const ItemOutcome outcome = remove_one(L"F:\\cache");
  EXPECT_EQ(outcome.disposition, Disposition::removed);
  EXPECT_FALSE(fs().exists(L"F:\\cache"));
  EXPECT_TRUE(fs().exists(L"C:\\Windows\\System32\\kernel32.dll"));
  EXPECT_FALSE(touched(L"C:\\Windows"));
}

TEST_F(SafeRemoveTest, ReparsePointItemRemovedAsLink) {
  fs().add_directory(L"F:\\proj\\node_modules\\.bin", reparse);
  const ItemOutcome outcome = remove_one(L"F:\\proj\\node_modules\\.bin");
  EXPECT_EQ(outcome.disposition, Disposition::removed);
  EXPECT_EQ(fs().operations(), (std::vector<fakes::FakeFileSystem::Operation>{
                                   {.kind = fakes::FakeFileSystem::OperationKind::remove,
                                    .from = L"F:\\proj\\node_modules\\.bin"}}));
}

TEST_F(SafeRemoveTest, CloudPlaceholderSkippedWithoutTouchingIt) {
  const std::vector<os::FileAttributes> placeholders = {
      {.recall_on_data_access = true}, {.recall_on_open = true}, {.offline = true}};
  std::vector<PlanItemId> ids;
  for (std::size_t index = 0; index < placeholders.size(); ++index) {
    const path item = L"C:\\Users\\owner\\OneDrive\\file" + std::to_wstring(index) + L".iso";
    fs().add_file(item, 4_GiB, placeholders[index]);
    ids.push_back(add_item(item));
  }
  for (const ItemOutcome& outcome : remove(ids)) {
    EXPECT_EQ(outcome.disposition, Disposition::skipped);
    EXPECT_EQ(outcome.reason, "cloud_placeholder");
    EXPECT_EQ(outcome.freed, Bytes{});
  }
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, PlaceholderInsideFolderKeepsTheFolder) {
  fs().add_file(L"F:\\cache\\local.bin", 1_KiB);
  fs().add_file(L"F:\\cache\\cloud.bin", 1_MiB, {.recall_on_open = true});
  const ItemOutcome outcome = remove_one(L"F:\\cache");
  EXPECT_EQ(outcome.disposition, Disposition::partial);
  EXPECT_EQ(outcome.reason, "cloud_placeholder");
  EXPECT_EQ(outcome.freed, 1_KiB);
  EXPECT_FALSE(fs().exists(L"F:\\cache\\local.bin"));
  EXPECT_TRUE(fs().exists(L"F:\\cache\\cloud.bin"));
  EXPECT_TRUE(fs().exists(L"F:\\cache"));
}

TEST_F(SafeRemoveTest, DeniedEntryInsideFolderKeptWithItsParents) {
  fs().add_file(L"F:\\proj\\vendor\\lib\\.git\\HEAD", 1_KiB);
  fs().add_file(L"F:\\proj\\vendor\\lib\\lib.c", 2_KiB);
  fs().add_file(L"F:\\proj\\vendor\\other\\other.c", 4_KiB);
  const ItemOutcome outcome = remove_one(L"F:\\proj\\vendor");
  EXPECT_EQ(outcome.disposition, Disposition::partial);
  EXPECT_EQ(outcome.reason, "deny_list:git");
  EXPECT_EQ(outcome.freed, 6_KiB);
  EXPECT_TRUE(fs().exists(L"F:\\proj\\vendor\\lib\\.git\\HEAD"));
  EXPECT_FALSE(fs().exists(L"F:\\proj\\vendor\\lib\\lib.c"));
  EXPECT_FALSE(fs().exists(L"F:\\proj\\vendor\\other"));
  EXPECT_FALSE(touched(L"F:\\proj\\vendor\\lib\\.git"));
}

TEST_F(SafeRemoveTest, NestedFolderRemovedCompletely) {
  fs().add_file(L"F:\\build\\obj\\a\\b\\c.o", 1_KiB);
  fs().add_file(L"F:\\build\\obj\\d.o", 2_KiB);
  fs().add_directory(L"F:\\build\\obj\\empty");
  const ItemOutcome outcome = remove_one(L"F:\\build\\obj");
  EXPECT_EQ(outcome.disposition, Disposition::removed);
  EXPECT_EQ(outcome.freed, 3_KiB);
  EXPECT_FALSE(fs().exists(L"F:\\build\\obj"));
  EXPECT_TRUE(fs().exists(L"F:\\build"));
}

TEST_F(SafeRemoveTest, ItemNotInPlanRefused) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  fs().add_file(L"F:\\tmp\\b.tmp", 1_KiB);
  add_item(L"F:\\tmp\\a.tmp");
  add_item(L"F:\\tmp\\b.tmp");
  const std::vector<ItemOutcome> outcomes = remove({PlanItemId{3}});
  ASSERT_EQ(outcomes.size(), 1U);
  EXPECT_EQ(outcomes[0].item, PlanItemId{3});
  expect_refused(outcomes[0], "not_in_plan");
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, PlanThatWasNeverStoredRefused) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  const PlanItemId item = add_item(L"F:\\tmp\\a.tmp");
  const std::vector<PlanItemId> items = {item};
  expect_refused(safe_remove().remove(PlanId{7}, items).front(), "not_in_plan");
  EXPECT_TRUE(fs().operations().empty());
}

TEST(SafeRemoveApi, AcceptsNoRawPath) {
  static_assert(!std::is_invocable_v<decltype(&SafeRemove::remove), SafeRemove&, path>);
  static_assert(!std::is_invocable_v<decltype(&SafeRemove::remove), SafeRemove&, PlanId,
                                     std::span<const path>>);
  static_assert(std::is_invocable_v<decltype(&SafeRemove::remove), SafeRemove&, PlanId,
                                    std::span<const PlanItemId>>);
}

TEST_F(SafeRemoveTest, PlanNotApplyingRefused) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  const PlanItemId item = add_item(L"F:\\tmp\\a.tmp");
  plan().state = PlanState::awaiting_approval;
  expect_refused(remove({item}).front(), "plan_not_applying");
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, ItemTheOwnerRejectedRefused) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  const PlanItemId item = add_item(L"F:\\tmp\\a.tmp");
  plan().items.back().approval = ItemApproval::not_approved;
  expect_refused(remove({item}).front(), "not_approved");
  EXPECT_TRUE(fs().operations().empty());
}

TEST_F(SafeRemoveTest, FolderSwappedForJunctionRefused) {
  fs().add_file(L"F:\\proj\\target\\obj\\a.o", 1_KiB);
  fs().add_file(L"C:\\Users\\owner\\Documents\\thesis.docx", 1_KiB);
  const PlanItemId item = add_item(L"F:\\proj\\target");
  // After the plan was stored, the folder became a junction to the owner's Documents.
  fs().erase(L"F:\\proj\\target");
  fs().add_directory(L"F:\\proj\\target", reparse);
  expect_refused(remove({item}).front(), "changed_since_plan");
  EXPECT_TRUE(fs().operations().empty());
  EXPECT_TRUE(fs().exists(L"C:\\Users\\owner\\Documents\\thesis.docx"));
}

TEST_F(SafeRemoveTest, FileRecreatedWithAnotherIdentityRefused) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  const PlanItemId item = add_item(L"F:\\tmp\\a.tmp");
  fs().erase(L"F:\\tmp\\a.tmp");
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  expect_refused(remove({item}).front(), "changed_since_plan");
  EXPECT_TRUE(fs().exists(L"F:\\tmp\\a.tmp"));
}

TEST_F(SafeRemoveTest, MixedPlanGetsOneOutcomePerItem) {
  fs().add_file(L"C:\\Users\\owner\\AppData\\Local\\Temp\\a.tmp", 4_KiB);
  fs().add_file(L"C:\\Windows\\System32\\config\\SYSTEM", 1_MiB);
  fs().add_file(L"C:\\Users\\owner\\OneDrive\\big.iso", 4_GiB, {.recall_on_data_access = true});
  const std::vector<ItemOutcome> outcomes = remove({
      add_item(L"C:\\Users\\owner\\AppData\\Local\\Temp\\a.tmp"),
      add_item(L"C:\\Windows\\System32\\config\\SYSTEM"),
      add_item(L"C:\\Users\\owner\\OneDrive\\big.iso"),
  });
  ASSERT_EQ(outcomes.size(), 3U);
  EXPECT_EQ(outcomes[0].disposition, Disposition::removed);
  EXPECT_EQ(outcomes[0].freed, 4_KiB);
  EXPECT_EQ(outcomes[1].disposition, Disposition::refused);
  EXPECT_EQ(outcomes[1].freed, Bytes{});
  EXPECT_EQ(outcomes[2].disposition, Disposition::skipped);
  EXPECT_EQ(outcomes[2].freed, Bytes{});
}

TEST_F(SafeRemoveTest, OperatingSystemFailureDoesNotStopTheRest) {
  fs().add_file(L"F:\\tmp\\locked.tmp", 1_KiB);
  fs().add_file(L"F:\\tmp\\free.tmp", 1_KiB);
  fs().fail_on(L"F:\\tmp\\locked.tmp", Errc::in_use);
  const std::vector<ItemOutcome> outcomes =
      remove({add_item(L"F:\\tmp\\locked.tmp"), add_item(L"F:\\tmp\\free.tmp")});
  EXPECT_EQ(outcomes[0].disposition, Disposition::failed);
  EXPECT_EQ(outcomes[0].reason, "in_use");
  EXPECT_EQ(outcomes[1].disposition, Disposition::removed);
}

TEST_F(SafeRemoveTest, VanishedItemSkipped) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  const PlanItemId item = add_item(L"F:\\tmp\\a.tmp");
  fs().erase(L"F:\\tmp\\a.tmp");
  const ItemOutcome outcome = remove({item}).front();
  EXPECT_EQ(outcome.disposition, Disposition::skipped);
  EXPECT_EQ(outcome.reason, "not_found");
}

TEST_F(SafeRemoveTest, MovesFileWithinVolumeFreeingNothing) {
  const FileIdentity identity = fs().add_file(L"F:\\proj\\big.log", 1_MiB);
  const ItemOutcome outcome =
      move({{.item = add_item(L"F:\\proj\\big.log"),
             .destination = L"F:\\.myclean-quarantine\\run-1\\proj\\big.log"}})
          .front();
  EXPECT_EQ(outcome.disposition, Disposition::moved);
  EXPECT_EQ(outcome.freed, Bytes{});
  EXPECT_FALSE(fs().exists(L"F:\\proj\\big.log"));
  EXPECT_EQ(fs().stat(L"F:\\.myclean-quarantine\\run-1\\proj\\big.log")->identity, identity);
}

TEST_F(SafeRemoveTest, MovesFileToAnotherVolumeFreeingItsSize) {
  fs().add_file(L"C:\\Users\\owner\\Downloads\\old.iso", 1_MiB);
  fs().add_directory(L"L:\\");
  const ItemOutcome outcome = move({{.item = add_item(L"C:\\Users\\owner\\Downloads\\old.iso"),
                                     .destination = L"L:\\.myclean-quarantine\\run-1\\old.iso"}})
                                  .front();
  EXPECT_EQ(outcome.disposition, Disposition::moved);
  EXPECT_EQ(outcome.freed, 1_MiB);
  EXPECT_TRUE(fs().exists(L"L:\\.myclean-quarantine\\run-1\\old.iso"));
}

TEST_F(SafeRemoveTest, MovesFolderByRenaming) {
  fs().add_file(L"F:\\proj\\out\\a.o", 1_KiB);
  const ItemOutcome outcome =
      move({{.item = add_item(L"F:\\proj\\out"), .destination = L"F:\\.myclean-quarantine\\out"}})
          .front();
  EXPECT_EQ(outcome.disposition, Disposition::moved);
  EXPECT_TRUE(fs().exists(L"F:\\.myclean-quarantine\\out\\a.o"));
  EXPECT_EQ(fs().operations().back().kind, fakes::FakeFileSystem::OperationKind::rename);
}

TEST_F(SafeRemoveTest, MoveIntoDeniedDestinationRefused) {
  fs().add_file(L"F:\\tmp\\payload.dll", 1_KiB);
  fs().add_directory(L"C:\\Program Files\\App");
  fs().set_short_name(L"C:\\Program Files", L"PROGRA~1");
  const PlanItemId item = add_item(L"F:\\tmp\\payload.dll");
  const std::vector<ItemOutcome> outcomes = move({
      {.item = item, .destination = L"C:\\Windows\\System32\\payload.dll"},
      {.item = item, .destination = L"C:\\PROGRA~1\\App\\new\\payload.dll"},
      {.item = item, .destination = L"C:\\Users\\owner\\Desktop\\payload.dll"},
      {.item = item, .destination = L"payload.dll"},
  });
  expect_refused(outcomes[0], "deny_list:windir");
  expect_refused(outcomes[1], "deny_list:program_files");
  expect_refused(outcomes[2], "deny_list:user_known_folder");
  expect_refused(outcomes[3], "invalid_path");
  EXPECT_TRUE(fs().operations().empty());
  EXPECT_TRUE(fs().exists(L"F:\\tmp\\payload.dll"));
}

TEST_F(SafeRemoveTest, MoveAppliesTheSourceGuards) {
  fs().add_file(L"C:\\Users\\owner\\Documents\\thesis.docx", 1_KiB);
  fs().add_file(L"C:\\Users\\owner\\OneDrive\\big.iso", 4_GiB, {.offline = true});
  const std::vector<ItemOutcome> outcomes = move({
      {.item = add_item(L"C:\\Users\\owner\\Documents\\thesis.docx"),
       .destination = L"C:\\.myclean-quarantine\\thesis.docx"},
      {.item = add_item(L"C:\\Users\\owner\\OneDrive\\big.iso"),
       .destination = L"C:\\.myclean-quarantine\\big.iso"},
      {.item = PlanItemId{99}, .destination = L"C:\\.myclean-quarantine\\x"},
  });
  expect_refused(outcomes[0], "deny_list:user_known_folder");
  EXPECT_EQ(outcomes[1].disposition, Disposition::skipped);
  EXPECT_EQ(outcomes[1].reason, "cloud_placeholder");
  expect_refused(outcomes[2], "not_in_plan");
  EXPECT_TRUE(fs().operations().empty());
}

TEST(SafeRemoveCreate, RequiresValidLocations) {
  fakes::FakeFileSystem fs;
  const StoredPlans plans;
  ProtectedLocations missing_windows = owner_locations();
  missing_windows.windows.clear();
  EXPECT_EQ(SafeRemove::create(fs, plans, missing_windows).error().code, Errc::invalid_argument);
  ProtectedLocations relative_folder = owner_locations();
  relative_folder.owner_known_folders.emplace_back(L"Documents");
  EXPECT_EQ(SafeRemove::create(fs, plans, relative_folder).error().code, Errc::invalid_argument);
  EXPECT_TRUE(SafeRemove::create(fs, plans, owner_locations()).has_value());
}

TEST(Disposition, TextFormsRoundTrip) {
  for (const Disposition disposition :
       {Disposition::removed, Disposition::moved, Disposition::partial, Disposition::skipped,
        Disposition::refused, Disposition::failed}) {
    EXPECT_EQ(parse_disposition(to_string(disposition)), disposition);
  }
  EXPECT_FALSE(parse_disposition("deleted").has_value());
}

}  // namespace
}  // namespace myclean::safety
