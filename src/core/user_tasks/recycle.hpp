#pragma once

#include <filesystem>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/model/action.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"

// The owner's "Send to Recycle Bin" of report-view items (a duplicate, an old download; plan kind
// cleanup, action RecycleItems, made by plans.recycle). The tray runs it with the owner's token,
// through the shell, so that the owner can restore every file from the Recycle Bin.
namespace myclean::user_tasks {

// One file the owner chose, as the plan recorded it.
struct RecycleItem {
  PlanItemId id;
  std::filesystem::path path;
  std::optional<FileIdentity> identity;  // nullopt: the plan recorded none, so it is refused
  Bytes size;
};

// What sending files to the Recycle Bin works with. Every pointer must outlive the action.
struct RecycleTools {
  cleanup::IRecycler* recycler = nullptr;
  const os::IFileSystem* file_system = nullptr;
  const safety::ProtectedLocations* locations = nullptr;  // what the deny-list protects
};

// Sends the owner's report-view items to the Recycle Bin (tier report, owner's context):
//   preview  the items that are still the file the plan recorded (same volume serial and file
//            id), a regular file that is neither a reparse point nor a cloud placeholder, and in
//            no deny-list place; each other item is left out with its reason (skipped());
//   apply    checks each previewed item the same way again, then recycles them. Undo data: the
//            path of each recycled item; not_reversible "restore them from the Recycle Bin", since
//            only the shell can put them back. Fails when none was recycled;
//   verify   metric "files_left": how many recycled items are still at their path (better when
//            fewer than were sent);
//   undo     always fails with not_reversible.
class RecycleItems final : public Action {
 public:
  RecycleItems(RecycleTools tools, std::vector<RecycleItem> items, std::stop_token stop = {})
      : tools_(tools), items_(std::move(items)), stop_(std::move(stop)) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

  // The items left out and why, by path, of the last preview and apply.
  [[nodiscard]] const std::vector<cleanup::Skipped>& skipped() const { return skipped_; }

 private:
  // Why `item` may not be recycled now; success when it may.
  [[nodiscard]] Result<void> check(const RecycleItem& item) const;

  RecycleTools tools_;
  std::vector<RecycleItem> items_;
  std::stop_token stop_;
  std::vector<cleanup::Skipped> skipped_;
};

}  // namespace myclean::user_tasks
