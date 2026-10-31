#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/plan_source.hpp"

namespace myclean::safety {

// What happened to one requested item.
enum class Disposition {
  removed,  // the item is gone
  moved,    // the item is at its destination
  partial,  // a folder: its removable entries are gone, the folder is kept for the rest
  skipped,  // left alone on purpose: a cloud placeholder, or already gone
  refused,  // refused by a safety guard before anything was touched
  failed,   // the operating system refused the change
};

[[nodiscard]] std::string_view to_string(Disposition disposition);
[[nodiscard]] std::optional<Disposition> parse_disposition(std::string_view text);

// The outcome of one item. Reasons are fixed codes the audit log stores:
//   not_in_plan             the plan was never stored, or does not contain the item
//   plan_not_applying       the plan is not in state `applying`
//   not_approved            the owner did not approve the item
//   invalid_path            the path (or the destination) is not an absolute drive path
//   deny_list:<entry>       a deny-list entry (deny_list()) matched the path or destination
//   reparse_point_in_path   a folder above the item is a junction or link
//   changed_since_plan      the entry's identity differs from what the plan recorded
//   cloud_placeholder       a cloud file whose content is not (fully) local
//   not_found               the item no longer exists
//   <Errc name>             an operating-system failure, such as access_denied or in_use
// A partial folder carries the reason of the first entry it kept.
struct ItemOutcome {
  PlanItemId item;
  std::filesystem::path path;  // as the plan recorded it; empty when the item is not in the plan
  Disposition disposition = Disposition::refused;
  std::string reason;  // empty when removed or moved
  std::string detail;  // human-readable specifics, such as the operating-system message
  Bytes freed;         // size on disk released on the item's volume
};

// Where one item of a plan goes.
struct MoveRequest {
  PlanItemId item;
  std::filesystem::path destination;  // the item's new full path; must not exist yet
};

// The only code path that deletes or moves files (docs/architecture.md section 6).
//
// It acts only on items of a plan read from IPlanSource, while that plan is `applying`, and
// only on items the owner did not reject. Right before acting on each item it normalizes the
// path (case, "..", "\\?\", 8.3 names, trailing dots), applies the hard deny-list, refuses a
// path that runs through a reparse point, checks that the entry's identity still matches the
// plan, and skips cloud placeholders. It never follows a reparse point: a junction or link is
// removed as a link and never entered. Every item gets an outcome; one refusal does not stop
// the others.
class SafeRemove {
 public:
  // Fails with invalid_argument when `locations.windows` or `locations.profiles` is missing or
  // any location is not an absolute drive path.
  [[nodiscard]] static Result<SafeRemove> create(os::IFileSystem& file_system,
                                                 const IPlanSource& plans,
                                                 ProtectedLocations locations);

  // Deletes the given items of stored plan `plan`: a folder with everything inside it that
  // passes the same guards, a reparse point as the link itself.
  [[nodiscard]] std::vector<ItemOutcome> remove(PlanId plan, std::span<const PlanItemId> items);

  // Moves the given items of stored plan `plan`, creating the destination's parent folders.
  // The destination passes the same normalization and deny-list. A folder moves only within its
  // volume; a file may move to another volume, which frees its size on the source.
  [[nodiscard]] std::vector<ItemOutcome> move(PlanId plan, std::span<const MoveRequest> requests);

 private:
  struct Verdict {
    Disposition disposition;
    std::string reason;
    std::string detail;
  };
  struct Target {
    std::filesystem::path path;  // normalized and long-named
    os::FileStat stat;
  };
  struct Run {
    Result<Plan> plan;
    ProtectedLocations locations;  // long-named for this run
  };
  struct Tally {
    Bytes freed;
    int removed = 0;
    std::optional<Verdict> first_kept;

    // Records the first reason something stays; true when `verdict` holds one. An entry
    // someone else removed meanwhile does not stay.
    bool keep(std::optional<Verdict> verdict) {
      if (!verdict || verdict->reason == "not_found") {
        return false;
      }
      if (!first_kept) {
        first_kept = std::move(verdict);
      }
      return true;
    }
  };

  SafeRemove(os::IFileSystem& file_system, const IPlanSource& plans, ProtectedLocations locations);

  // An operating-system error as a verdict; on the source, a missing entry is a skip.
  [[nodiscard]] static Verdict failure(const Error& error);
  [[nodiscard]] static Verdict source_failure(const Error& error);

  [[nodiscard]] Run begin(PlanId plan) const;
  [[nodiscard]] Result<std::filesystem::path> long_form(const std::filesystem::path& path) const;
  [[nodiscard]] static tl::expected<const PlanItem*, Verdict> find_item(const Run& run,
                                                                        PlanItemId id);
  [[nodiscard]] tl::expected<Target, Verdict> check_source(const Run& run,
                                                           const PlanItem& item) const;
  [[nodiscard]] tl::expected<std::filesystem::path, Verdict> check_destination(
      const Run& run, const std::filesystem::path& destination) const;
  [[nodiscard]] tl::expected<os::FileStat, Verdict> current_stat(
      const std::filesystem::path& path, const FileIdentity& expected) const;
  // The current state of an entry inside a folder being emptied, or the verdict that keeps it.
  [[nodiscard]] tl::expected<os::FileStat, Verdict> check_entry(const Run& run,
                                                                const os::DirEntry& entry) const;
  // Removes `path` if it is still the entry `expected`; a vanished entry counts as removed.
  [[nodiscard]] std::optional<Verdict> remove_checked(const std::filesystem::path& path,
                                                      const FileIdentity& expected);
  [[nodiscard]] Tally remove_contents(const Run& run, const std::filesystem::path& folder);
  [[nodiscard]] ItemOutcome remove_item(const Run& run, PlanItemId id);
  [[nodiscard]] ItemOutcome move_item(const Run& run, const MoveRequest& request);

  os::IFileSystem* file_system_;
  const IPlanSource* plans_;
  ProtectedLocations locations_;  // normalized
};

}  // namespace myclean::safety
