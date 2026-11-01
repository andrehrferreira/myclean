#include "core/safety/safe_remove.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

  // Adds a 1 KiB file for every new path with an extension and a folder for every other new
  // one, plans them all and expects each to be refused with `reason`, untouched.
  void expect_all_refused(const std::vector<path>& items, std::string_view reason) {
    std::vector<PlanItemId> ids;
    for (const path& item : items) {
      const bool exists = fs_.exists(item);  // created as the parent of an earlier item
      if (!exists && item.has_extension()) {
        fs_.add_file(item, 1_KiB);
      } else if (!exists) {
        fs_.add_directory(item);
      }
      ids.push_back(add_item(item));
    }
    for (const ItemOutcome& outcome : remove(ids)) {
      expect_refused(outcome, reason);
    }
    EXPECT_TRUE(fs_.operations().empty());
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

TEST_F(SafeRemoveDenyList, SystemVolumeInformationRefusedOnEveryVolume) {
  expect_all_refused(
      {L"C:\\System Volume Information", L"D:\\system volume information\\tracking.log",
       L"E:\\System Volume Information\\{3808876b-c176-4e48-b7ae-04046e6cc752}"},
      "deny_list:system_volume_information");
}

TEST_F(SafeRemoveDenyList, RecoveryRefusedAtVolumeRootOnly) {
  expect_all_refused({L"C:\\Recovery", L"D:\\RECOVERY\\WindowsRE\\Winre.wim"},
                     "deny_list:recovery");
  fs().add_file(L"F:\\proj\\Recovery\\notes.txt", 1_KiB);
  EXPECT_EQ(remove_one(L"F:\\proj\\Recovery\\notes.txt").disposition, Disposition::removed);
}

TEST_F(SafeRemoveDenyList, BootRefusedAtVolumeRootOnly) {
  expect_all_refused({L"C:\\Boot", L"C:\\boot\\BCD", L"D:\\Boot\\memtest.exe"}, "deny_list:boot");
  fs().add_file(L"F:\\spring\\boot\\app.jar", 1_KiB);
  EXPECT_EQ(remove_one(L"F:\\spring\\boot\\app.jar").disposition, Disposition::removed);
}

TEST_F(SafeRemoveDenyList, PageFileRefusedAtEveryVolumeRoot) {
  expect_all_refused(
      {L"C:\\pagefile.sys", L"C:\\hiberfil.sys", L"C:\\swapfile.sys", L"D:\\PAGEFILE.SYS"},
      "deny_list:page_file");
  fs().add_file(L"F:\\backup\\pagefile.sys", 1_KiB);
  EXPECT_EQ(remove_one(L"F:\\backup\\pagefile.sys").disposition, Disposition::removed);
}

TEST_F(SafeRemoveDenyList, RootDollarEntryRefused) {
  expect_all_refused({L"C:\\$Recycle.Bin\\S-1-5-21-1\\$R0A1B2C.tmp", L"C:\\$Recycle.Bin",
                      L"C:\\$WinREAgent\\Scratch", L"C:\\$WinREAgent", L"D:\\$Extend",
                      L"C:\\$Windows.~BT\\Sources\\setup.exe"},
                     "deny_list:root_dollar_entry");
  fs().add_file(L"F:\\proj\\$tmp\\a.tmp", 1_KiB);
  EXPECT_EQ(remove_one(L"F:\\proj\\$tmp\\a.tmp").disposition, Disposition::removed);
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
                                     std::span<const path>, IProgress*>);
  static_assert(std::is_invocable_v<decltype(&SafeRemove::remove), SafeRemove&, PlanId,
                                    std::span<const PlanItemId>, IProgress*>);
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

TEST_F(SafeRemoveTest, SwapBetweenCheckAndDeleteRefused) {
  fs().add_file(L"F:\\tmp\\a.tmp", 1_KiB);
  fs().add_file(L"C:\\Users\\owner\\Documents\\thesis.docx", 1_KiB);
  const PlanItemId item = add_item(L"F:\\tmp\\a.tmp");
  // Every check passed; right before the delete the file becomes a junction to Documents.
  fs().on_remove([this](const path& removed) {
    if (removed == path{L"F:\\tmp\\a.tmp"} && !fs().stat(removed)->attributes.reparse_point) {
      fs().erase(removed);
      fs().add_directory(removed, reparse);
    }
  });
  expect_refused(remove({item}).front(), "changed_since_plan");
  EXPECT_TRUE(fs().stat(L"F:\\tmp\\a.tmp")->attributes.reparse_point);
  EXPECT_TRUE(fs().exists(L"C:\\Users\\owner\\Documents\\thesis.docx"));
}

TEST_F(SafeRemoveTest, SwapInsideFolderBetweenCheckAndDeleteKeepsTheNewEntry) {
  fs().add_file(L"F:\\cache\\a.bin", 1_KiB);
  fs().add_file(L"F:\\cache\\b.bin", 2_KiB);
  const PlanItemId item = add_item(L"F:\\cache");
  fs().on_remove([this](const path& removed) {
    if (removed == path{L"F:\\cache\\a.bin"} && fs().stat(removed)->size == 1_KiB) {
      fs().erase(removed);
      fs().add_file(removed, 3_KiB);
    }
  });
  const ItemOutcome outcome = remove({item}).front();
  EXPECT_EQ(outcome.disposition, Disposition::partial);
  EXPECT_EQ(outcome.reason, "changed_since_plan");
  EXPECT_EQ(outcome.freed, 2_KiB);
  EXPECT_EQ(fs().stat(L"F:\\cache\\a.bin")->size, 3_KiB);
  EXPECT_FALSE(fs().exists(L"F:\\cache\\b.bin"));
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

// A folder of C: moved to L:, as a quarantine on a volume short of space does it.
class SafeRemoveAcrossVolumes : public SafeRemoveTest {
 protected:
  using Kind = fakes::FakeFileSystem::OperationKind;

  SafeRemoveAcrossVolumes() {
    fs().add_directory(L"C:\\cache\\zig\\empty");
    fs().add_file(L"C:\\cache\\zig\\o\\a.bin", 1_MiB);
    b_ = fs().add_file(L"C:\\cache\\zig\\o\\sub\\b.bin", 2_MiB);
    fs().add_directory(L"L:\\");
  }

  ItemOutcome move_zig(std::function<Result<void>(const FileIdentity&)> on_copied = {}) {
    return move({{.item = add_item(L"C:\\cache\\zig"),
                  .destination = destination(),
                  .on_copied = std::move(on_copied)}})
        .front();
  }

  [[nodiscard]] std::size_t count(Kind kind) {
    return static_cast<std::size_t>(
        std::ranges::count(fs().operations(), kind, &fakes::FakeFileSystem::Operation::kind));
  }

  // The source is where it was, every file with the identity it had.
  void expect_source_intact(const FileIdentity& a, bool b_identity_kept = true) {
    EXPECT_EQ(fs().stat(L"C:\\cache\\zig\\o\\a.bin")->identity, a);
    ASSERT_TRUE(fs().exists(L"C:\\cache\\zig\\o\\sub\\b.bin"));
    EXPECT_EQ(fs().stat(L"C:\\cache\\zig\\o\\sub\\b.bin")->size, 2_MiB);
    if (b_identity_kept) {
      EXPECT_EQ(fs().stat(L"C:\\cache\\zig\\o\\sub\\b.bin")->identity, b_);
    }
    EXPECT_TRUE(fs().exists(L"C:\\cache\\zig\\empty"));
    EXPECT_FALSE(fs().exists(destination()));
  }

  [[nodiscard]] const path& destination() const { return destination_; }

 private:
  path destination_{L"L:\\.myclean-quarantine\\1\\1\\zig"};
  FileIdentity b_;
};

TEST_F(SafeRemoveAcrossVolumes, MovesAFolderCompletelyAndFlushesOnce) {
  const ItemOutcome outcome = move_zig();
  EXPECT_EQ(outcome.disposition, Disposition::moved) << outcome.reason << outcome.detail;
  EXPECT_EQ(outcome.freed, 3_MiB);  // left C: at once
  EXPECT_FALSE(fs().exists(L"C:\\cache\\zig"));
  EXPECT_EQ(fs().stat(destination() / L"o\\a.bin")->size, 1_MiB);
  EXPECT_EQ(fs().stat(destination() / L"o\\sub\\b.bin")->size, 2_MiB);
  EXPECT_EQ(fs().stat(destination() / L"empty")->kind, os::EntryKind::directory);
  EXPECT_EQ(count(Kind::copy_file), 2U);
  EXPECT_EQ(count(Kind::flush), 1U);  // once per item, not per file
  EXPECT_EQ(count(Kind::rename), 0U);
  // Every copy is made and flushed before the first source entry goes.
  const auto first_remove =
      std::ranges::find(fs().operations(), Kind::remove, &fakes::FakeFileSystem::Operation::kind);
  const auto flush =
      std::ranges::find(fs().operations(), Kind::flush, &fakes::FakeFileSystem::Operation::kind);
  EXPECT_LT(flush, first_remove);
}

TEST_F(SafeRemoveAcrossVolumes, AFileInUseLeavesTheWholeFolderWhereItWas) {
  const FileIdentity a = fs().stat(L"C:\\cache\\zig\\o\\a.bin")->identity;
  fs().fail_on(L"C:\\cache\\zig\\o\\sub\\b.bin", Errc::in_use, Kind::copy_file);
  const ItemOutcome outcome = move_zig();
  EXPECT_EQ(outcome.disposition, Disposition::failed);
  EXPECT_EQ(outcome.reason, "in_use");
  EXPECT_EQ(outcome.freed, Bytes{});
  expect_source_intact(a);
  EXPECT_EQ(count(Kind::flush), 0U);
  // Only copies were removed: nothing of the source.
  for (const fakes::FakeFileSystem::Operation& op : fs().operations()) {
    if (op.kind == Kind::remove) {
      EXPECT_TRUE(op.from.native().starts_with(L"L:\\")) << op.from;
    }
  }
}

TEST_F(SafeRemoveAcrossVolumes, AFailedRemovalCopiesBackWhatWasRemoved) {
  // The source goes deepest first: b.bin and its folder are removed before a.bin fails.
  const FileIdentity a = fs().stat(L"C:\\cache\\zig\\o\\a.bin")->identity;
  fs().fail_on(L"C:\\cache\\zig\\o\\a.bin", Errc::access_denied, Kind::remove);
  const ItemOutcome outcome = move_zig();
  EXPECT_EQ(outcome.disposition, Disposition::failed);
  EXPECT_EQ(outcome.reason, "access_denied");
  EXPECT_EQ(outcome.freed, Bytes{});
  expect_source_intact(a, false);     // b.bin is a copy put back
  EXPECT_EQ(count(Kind::flush), 2U);  // the copy, then what was put back
}

TEST_F(SafeRemoveAcrossVolumes, ACopyThatCannotBePutBackStaysAtTheDestination) {
  fs().fail_on(L"C:\\cache\\zig\\o\\a.bin", Errc::access_denied, Kind::remove);
  // b.bin is put back, but its flush on C: fails: the complete copy on L: is kept.
  fs().fail_on(L"C:\\cache\\zig\\o\\sub\\b.bin", Errc::io_error, Kind::flush);
  const ItemOutcome outcome = move_zig();
  EXPECT_EQ(outcome.disposition, Disposition::failed);
  EXPECT_NE(outcome.detail.find("stays complete at"), std::string::npos) << outcome.detail;
  EXPECT_TRUE(fs().exists(destination() / L"o\\a.bin"));
  EXPECT_TRUE(fs().exists(destination() / L"o\\sub\\b.bin"));
  EXPECT_TRUE(fs().exists(L"C:\\cache\\zig\\o\\a.bin"));
}

TEST_F(SafeRemoveAcrossVolumes, ALinkInsideIsNeverFollowedAndRefusesTheItem) {
  fs().add_directory(L"D:\\store\\pkg");
  fs().add_file(L"D:\\store\\pkg\\index.js", 1_KiB);
  fs().add_directory(L"C:\\cache\\zig\\o\\link", reparse);
  const ItemOutcome outcome = move_zig();
  expect_refused(outcome, "reparse_point_inside");
  EXPECT_EQ(outcome.detail, "C:\\cache\\zig\\o\\link");
  EXPECT_EQ(count(Kind::copy_file), 0U);
  EXPECT_EQ(count(Kind::remove), 0U);
  EXPECT_FALSE(touched(L"D:\\store"));
  EXPECT_TRUE(fs().exists(L"C:\\cache\\zig\\o\\link"));
}

TEST_F(SafeRemoveAcrossVolumes, APlaceholderInsideIsNeverHydratedAndSkipsTheItem) {
  fs().add_file(L"C:\\cache\\zig\\o\\cloud.bin", 1_GiB, {.recall_on_data_access = true});
  const ItemOutcome outcome = move_zig();
  EXPECT_EQ(outcome.disposition, Disposition::skipped);
  EXPECT_EQ(outcome.reason, "cloud_placeholder");
  EXPECT_EQ(count(Kind::copy_file), 0U);
  EXPECT_FALSE(touched(L"C:\\cache\\zig\\o\\cloud.bin"));
}

TEST_F(SafeRemoveAcrossVolumes, OnCopiedIsToldTheCopyBeforeTheSourceGoes) {
  std::optional<FileIdentity> told;
  bool source_there = false;
  const ItemOutcome outcome = move_zig([&](const FileIdentity& copy) -> Result<void> {
    told = copy;
    source_there = fs().exists(L"C:\\cache\\zig\\o\\a.bin");
    return {};
  });
  EXPECT_EQ(outcome.disposition, Disposition::moved);
  ASSERT_TRUE(told);
  EXPECT_EQ(*told, fs().stat(destination())->identity);
  EXPECT_TRUE(source_there);
}

TEST_F(SafeRemoveAcrossVolumes, AFailingOnCopiedLeavesTheItemWhereItWas) {
  const FileIdentity a = fs().stat(L"C:\\cache\\zig\\o\\a.bin")->identity;
  const ItemOutcome outcome = move_zig([](const FileIdentity&) -> Result<void> {
    return fail(Errc::io_error, "the store refused the record");
  });
  EXPECT_EQ(outcome.disposition, Disposition::failed);
  EXPECT_EQ(outcome.reason, "io_error");
  expect_source_intact(a);
}

TEST_F(SafeRemoveAcrossVolumes, AnExistingDestinationIsNeverMerged) {
  fs().add_file(destination() / L"o\\a.bin", 1_KiB);
  const ItemOutcome outcome = move_zig();
  EXPECT_EQ(outcome.disposition, Disposition::failed);
  EXPECT_EQ(outcome.reason, "already_exists");
  EXPECT_EQ(count(Kind::copy_file), 0U);
  EXPECT_EQ(fs().stat(destination() / L"o\\a.bin")->size, 1_KiB);
}

// Records what SafeRemove reports, as text.
class Recorder final : public IProgress {
 public:
  Recorder() = default;
  void begin(std::size_t items, Bytes bytes) override {
    events.push_back("begin " + std::to_string(items) + " " + std::to_string(bytes.count()));
  }
  void item_started(const PlanItem& item) override {
    events.push_back("start " + std::to_string(item.id.value()) + " " + to_utf8(item.path));
  }
  void file_done(Bytes bytes) override {
    events.push_back("file " + std::to_string(bytes.count()));
  }
  void item_finished(const ItemOutcome& outcome) override {
    events.push_back("end " + std::to_string(outcome.item.value()) + " " +
                     std::string(to_string(outcome.disposition)));
  }

  std::vector<std::string> events;
};

TEST_F(SafeRemoveAcrossVolumes, ProgressIsReportedInOrderPerItemAndFile) {
  fs().add_file(L"C:\\Temp\\x.tmp", 4_KiB);
  const PlanItemId zig = add_item(L"C:\\cache\\zig");
  const PlanItemId tmp = add_item(L"C:\\Temp\\x.tmp");
  plan().items[0].size = 3_MiB;
  plan().items[1].size = 4_KiB;
  Recorder removal;
  const std::vector<PlanItemId> items = {tmp, PlanItemId{99}};
  static_cast<void>(safe_remove().remove(plan_id, items, &removal));
  EXPECT_EQ(removal.events,
            (std::vector<std::string>{"begin 2 4096", "start 2 C:\\Temp\\x.tmp", "file 4096",
                                      "end 2 removed", "start 99 ", "end 99 refused"}));

  Recorder moving;
  const std::vector<MoveRequest> requests = {{.item = zig, .destination = destination()}};
  static_cast<void>(safe_remove().move(plan_id, requests, &moving));
  ASSERT_EQ(moving.events.size(), 5U);
  EXPECT_EQ(moving.events[0], "begin 1 " + std::to_string((3_MiB).count()));
  EXPECT_EQ(moving.events[1], "start 1 C:\\cache\\zig");
  // One report per file copied, in walk order.
  EXPECT_EQ(moving.events[2], "file " + std::to_string((1_MiB).count()));
  EXPECT_EQ(moving.events[3], "file " + std::to_string((2_MiB).count()));
  EXPECT_EQ(moving.events[4], "end 1 moved");
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
