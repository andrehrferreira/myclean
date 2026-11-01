#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
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
//   cloud_placeholder       a cloud file whose content is not (fully) local; for a folder moved
//                           to another volume, also one anywhere inside it
//   reparse_point_inside    a folder moved to another volume holds a junction or link
//   already_exists          the destination of a move exists
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
  // A folder moved to another volume: called once its copy at `destination` is complete and
  // durable, before any source entry is removed, with the copy's identity. A failure moves the
  // item back. Optional.
  std::function<Result<void>(const FileIdentity& copy)> on_copied;
};

// Told what SafeRemove does as it goes, on the thread that called remove() or move(). The calls
// must be quick and must not call SafeRemove.
class IProgress {
 public:
  virtual ~IProgress() = default;
  IProgress(const IProgress&) = delete;
  IProgress& operator=(const IProgress&) = delete;
  IProgress(IProgress&&) = delete;
  IProgress& operator=(IProgress&&) = delete;

  // A remove() or move() call starts on `items` items whose stored sizes add up to `bytes`.
  virtual void begin(std::size_t items, Bytes bytes) = 0;
  // `item` is handled now: the plan item as stored (only its id when the plan has none such).
  virtual void item_started(const PlanItem& item) = 0;
  // One file of the current item was removed, moved or copied, `bytes` on disk.
  virtual void file_done(Bytes bytes) = 0;
  // The current item ended with `outcome`.
  virtual void item_finished(const ItemOutcome& outcome) = 0;

 protected:
  IProgress() = default;
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
//
// A folder moved to another volume is copied and then removed, all or nothing. Every entry is
// checked as a removal checks it, and a junction, link or cloud placeholder anywhere inside
// leaves the whole item where it is (reparse_point_inside, cloud_placeholder): links are never
// recreated and placeholders never hydrated. Every file is copied with its attributes and
// timestamps, the copies are flushed once, on_copied is told, and only then are the sources
// removed through the guarded delete, deepest first. When a copy fails (a file in use, access
// denied), the copies are removed and the source is untouched; when a removal fails, what was
// removed is copied back and the destination removed, so the item ends where it was. A copy
// that cannot be put back stays at the destination: nothing is ever lost.
class SafeRemove {
 public:
  // Fails with invalid_argument when `locations.windows` or `locations.profiles` is missing or
  // any location is not an absolute drive path.
  [[nodiscard]] static Result<SafeRemove> create(os::IFileSystem& file_system,
                                                 const IPlanSource& plans,
                                                 ProtectedLocations locations);

  // Deletes the given items of stored plan `plan`: a folder with everything inside it that
  // passes the same guards, a reparse point as the link itself. `progress` (optional) is told
  // as it goes.
  [[nodiscard]] std::vector<ItemOutcome> remove(PlanId plan, std::span<const PlanItemId> items,
                                                IProgress* progress = nullptr);

  // Moves the given items of stored plan `plan`, creating the destination's parent folders.
  // The destination passes the same normalization and deny-list. On its own volume an item is
  // renamed; a file or a folder moved to another volume frees its size on the source.
  [[nodiscard]] std::vector<ItemOutcome> move(PlanId plan, std::span<const MoveRequest> requests,
                                              IProgress* progress = nullptr);

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
  // One entry inside a folder moved to another volume. Walk order puts every folder before the
  // entries inside it.
  struct Entry {
    std::filesystem::path relative;    // below the item
    os::FileStat stat;                 // of the source
    std::optional<FileIdentity> copy;  // of the copy at the destination, once made
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
  // Removes `path` only while it is still the entry `expected`, checked by the file system on
  // the handle that deletes. nullopt when removed; otherwise the verdict that keeps it
  // (changed_since_plan, or a skip with not_found when it vanished).
  [[nodiscard]] std::optional<Verdict> remove_checked(const std::filesystem::path& path,
                                                      const FileIdentity& expected);
  [[nodiscard]] Tally remove_contents(const Run& run, const std::filesystem::path& folder,
                                      IProgress& progress);
  [[nodiscard]] ItemOutcome remove_item(const Run& run, PlanItemId id, IProgress& progress);
  [[nodiscard]] ItemOutcome move_item(const Run& run, const MoveRequest& request,
                                      IProgress& progress);
  // Every entry inside `folder`, each checked as a removal checks it; otherwise the verdict that
  // keeps the whole item where it is.
  [[nodiscard]] tl::expected<std::vector<Entry>, Verdict> walk(
      const Run& run, const std::filesystem::path& folder) const;
  // Moves folder `source` to `destination` on another volume: nullopt when moved (`freed` set),
  // else the verdict, with the item where it was.
  [[nodiscard]] std::optional<Verdict> move_across(const Run& run, const Target& source,
                                                   const std::filesystem::path& destination,
                                                   const MoveRequest& request, IProgress& progress,
                                                   Bytes& freed);
  // The new, empty folder `destination` and its identity; it must not exist yet.
  [[nodiscard]] tl::expected<FileIdentity, Verdict> create_destination(
      const std::filesystem::path& destination);
  // Copies `entries` of folder `source` below `destination` in walk order (setting each copy),
  // then flushes the copies once; the verdict when something could not be copied.
  [[nodiscard]] std::optional<Verdict> copy_entries(const std::filesystem::path& source,
                                                    const std::filesystem::path& destination,
                                                    std::vector<Entry>& entries,
                                                    IProgress& progress);
  // Removes the copied `entries` of `source`, deepest first, then `source` itself; the verdict
  // when an entry stays, with entries[kept, size) removed.
  [[nodiscard]] std::optional<Verdict> remove_source(const Target& source,
                                                     std::span<const Entry> entries,
                                                     std::size_t& kept);
  // Removes what a move created: the copies of `entries` below `root`, last first, then `root`.
  // False when something stays.
  bool discard(const std::filesystem::path& root, const FileIdentity& identity,
               std::span<const Entry> entries);
  // Copies the removed `entries` of folder `source` back from its copy `copy`; false when one
  // could not be put back.
  bool put_back(const std::filesystem::path& source, const std::filesystem::path& copy,
                std::span<const Entry> entries);

  os::IFileSystem* file_system_;
  const IPlanSource* plans_;
  ProtectedLocations locations_;  // normalized
};

}  // namespace myclean::safety
