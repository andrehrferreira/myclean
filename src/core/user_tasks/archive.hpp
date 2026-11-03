#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/clean/probes.hpp"
#include "core/model/action.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/placement/relocation.hpp"
#include "core/safety/deny_list.hpp"

// Move to the HDD archive (owner-decisions spec): one of the owner's folders, cold for months, is
// copied to "<healthy HDD>\Archive\<letter>\<path>" (scanall::archive_folder), the copy is checked
// to hold the same files and bytes, and only then is the folder removed, through SafeRemove. The
// tray runs it with the owner's token, as the files are the owner's.
namespace myclean::user_tasks {

// What the owner approved: the folder and its archive folder.
struct Archive {
  std::filesystem::path source;
  std::filesystem::path target;
};

// What archiving works with. Every pointer must outlive the action.
struct ArchiveTools {
  const placement::Tools* placement = nullptr;            // copier, file_system and drives
  const clean::IUsage* usage = nullptr;                   // the folders of running processes
  const safety::ProtectedLocations* locations = nullptr;  // what the deny-list protects
};

// The identity a task item carries ({"volume_serial", "file_id": hex}); nullopt when the item
// has none (file_id empty or not 32 hex digits).
[[nodiscard]] std::optional<FileIdentity> parse_identity(std::uint64_t volume_serial,
                                                         std::string_view file_id);
// The 32 lower-case hex digits of `identity`'s file id.
[[nodiscard]] std::string file_id_text(const FileIdentity& identity);

// Archives one folder (tier ask, run in the owner's context):
//   preview  one item: the folder, its identity and size on disk;
//   apply    refuses, touching nothing, when the folder is not the one previewed (or not the one
//            the plan recorded, `expected`), is a scanall::system_location() or a deny-list
//            place, holds the current or image folder of a running process, when the target is
//            not the folder's archive folder on its volume, overlaps it, or its volume is not
//            healthy or lacks room (placement::check_target). Then copies the folder into the
//            empty or missing target and checks the copy holds the same files and bytes
//            (placement::copy_content), checks the folder still holds what was copied, and only
//            then removes it through SafeRemove (plan `plan`, the previewed item). A copy that
//            fails, or a removal that is refused or fails, removes the archive copy again, so
//            the folder stays where it was. A removal that keeps some entries (a cloud
//            placeholder, a git folder) succeeds: the archive holds them all. The undo data holds
//            both folders, what was copied and the archive copy's identity;
//   verify   metric "source_bytes": the folder's size on disk before and now (better when it
//            shrank); fails when the archive copy no longer holds what was copied;
//   undo     copies the archive back into the missing or empty folder (refusing when its volume
//            lacks the room, or when the folder holds something else), checks it, then removes
//            the archive copy through SafeRemove. A folder already holding exactly what was
//            archived is not copied again, so an undo whose last step failed can be retried.
class ArchiveAction final : public Action {
 public:
  ArchiveAction(ArchiveTools tools, PlanId plan, Archive archive,
                std::optional<FileIdentity> expected)
      : tools_(tools), plan_(plan), archive_(std::move(archive)), expected_(expected) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  // Why `source` (normalized) may not be archived; success when it may.
  [[nodiscard]] Result<void> check_source(const std::filesystem::path& source) const;
  // Removes `item` (a folder of this plan) through SafeRemove; the reason it stays otherwise.
  // `kept` is set when some entries stayed.
  [[nodiscard]] Result<void> remove(const PlanItem& item, bool& kept) const;
  // Removes the archive copy at `target`, made by this action, after a failed apply.
  void discard(const std::filesystem::path& target, PlanItemId item) const;

  ArchiveTools tools_;
  PlanId plan_;
  Archive archive_;
  std::optional<FileIdentity> expected_;
};

}  // namespace myclean::user_tasks
