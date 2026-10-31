// SafeRemove over the real Win32 file system, inside the sandbox. The protected folders of
// the deny-list (profiles, Documents, MyClean data) are sandbox folders here, so even a
// failure could not reach the owner's real files.
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/plan_source.hpp"
#include "core/safety/safe_remove.hpp"
#include "platform/fs/win32_file_system.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

using std::filesystem::path;

class OnePlan final : public safety::IPlanSource {
 public:
  OnePlan() = default;

  [[nodiscard]] Result<Plan> load(PlanId id) const override {
    if (id != plan.id) {
      return fail(Errc::not_found, "no such plan");
    }
    return plan;
  }

  Plan plan{.id = PlanId{1}, .proposer = Actor::scheduler(), .state = PlanState::applying};
};

class SafeRemoveOnDisk : public ::testing::Test {
 protected:
  SafeRemoveOnDisk() {
    write_file(documents_ / L"thesis.docx", 100);
    std::filesystem::create_directories(root_ / L"ProgramData\\MyClean");
  }

  PlanItemId add_item(const path& item) {
    const PlanItemId id{static_cast<std::int64_t>(plans_.plan.items.size()) + 1};
    plans_.plan.items.push_back(
        PlanItem{.id = id, .path = item, .identity = fs_.stat(item).value().identity});
    return id;
  }

  safety::ItemOutcome remove(PlanItemId item) {
    const safety::ProtectedLocations locations{
        .windows = path{L"C:\\Windows"},
        .profiles = root_ / L"Users",
        .owner_profile = root_ / L"Users\\owner",
        .owner_known_folders = {documents_},
        .myclean = {root_ / L"ProgramData\\MyClean"},
    };
    safety::SafeRemove remover = safety::SafeRemove::create(fs_, plans_, locations).value();
    const std::vector<PlanItemId> items = {item};
    return remover.remove(plans_.plan.id, items).front();
  }

  [[nodiscard]] const path& root() const { return root_; }
  [[nodiscard]] const path& documents() const { return documents_; }

 private:
  Sandbox sandbox_;
  path root_ = sandbox_.root();
  path documents_ = root_ / L"Users\\owner\\Documents";
  Win32FileSystem fs_;
  OnePlan plans_;
};

TEST_F(SafeRemoveOnDisk, FolderWithJunctionRemovedTargetIntact) {
  write_file(root() / L"store\\pkg\\index.js", 1000);
  write_file(root() / L"work\\node_modules\\real\\a.js", 1000);
  ASSERT_TRUE(make_junction(root() / L"work\\node_modules\\pkg", root() / L"store\\pkg"));
  ASSERT_TRUE(make_junction(root() / L"work\\node_modules\\docs", documents()));

  const safety::ItemOutcome outcome = remove(add_item(root() / L"work\\node_modules"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::removed) << outcome.reason;
  EXPECT_GT(outcome.freed, Bytes{});
  EXPECT_FALSE(entry_exists(root() / L"work\\node_modules"));
  EXPECT_TRUE(entry_exists(root() / L"store\\pkg\\index.js"));
  EXPECT_TRUE(entry_exists(documents() / L"thesis.docx"));
}

TEST_F(SafeRemoveOnDisk, FolderSwappedForJunctionRefused) {
  write_file(root() / L"work\\target\\obj\\a.o", 10);
  const PlanItemId item = add_item(root() / L"work\\target");
  std::filesystem::remove_all(root() / L"work\\target");
  ASSERT_TRUE(make_junction(root() / L"work\\target", documents()));

  const safety::ItemOutcome outcome = remove(item);
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "changed_since_plan");
  EXPECT_TRUE(entry_exists(root() / L"work\\target"));
  EXPECT_TRUE(entry_exists(documents() / L"thesis.docx"));
}

TEST_F(SafeRemoveOnDisk, ItemBehindAJunctionRefused) {
  write_file(root() / L"data\\file.txt", 10);
  ASSERT_TRUE(make_junction(root() / L"work\\link", root() / L"data"));
  // The identity is the real file's: only the junction in the path gives the detour away.
  const safety::ItemOutcome outcome = remove(add_item(root() / L"work\\link\\file.txt"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "reparse_point_in_path");
  EXPECT_TRUE(entry_exists(root() / L"data\\file.txt"));
}

TEST_F(SafeRemoveOnDisk, OfflineFileSkipped) {
  const path file = root() / L"work\\cloud.bin";
  write_file(file, 10);
  ASSERT_NE(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_OFFLINE), FALSE);
  const safety::ItemOutcome outcome = remove(add_item(file));
  EXPECT_EQ(outcome.disposition, safety::Disposition::skipped);
  EXPECT_EQ(outcome.reason, "cloud_placeholder");
  EXPECT_TRUE(entry_exists(file));
}

TEST_F(SafeRemoveOnDisk, DocumentsRefusedInAnySpelling) {
  std::wstring spelled = (documents() / L"thesis.docx").native();
  for (wchar_t& character : spelled) {
    character = static_cast<wchar_t>(::towupper(character));
  }
  const safety::ItemOutcome outcome = remove(add_item(spelled));
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "deny_list:user_known_folder");
  EXPECT_TRUE(entry_exists(documents() / L"thesis.docx"));
}

}  // namespace
}  // namespace myclean::platform
