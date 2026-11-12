#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The tray's reading of the cleaning, plan, quarantine and space methods (contracts in
// core/ipc/plans.hpp): the approval inbox (approvals spec, "Approval Inbox") and the Space view.
namespace myclean::tray_model {

// cleaning.status "applying": the plan or quarantine purge the service applies now.
struct ApplyingRow {
  std::string kind;                  // plan, or purge (a quarantine purge)
  std::optional<std::int64_t> plan;  // the plan, or the quarantine run deleted; none: scheduled
  std::string title;
  WallTime started_at;
  std::int64_t items_done = 0;
  std::int64_t items_total = 0;
  Bytes bytes_done;
  Bytes bytes_total;
  std::int64_t files_done = 0;
  std::string current;  // the item handled now; empty between items

  friend bool operator==(const ApplyingRow&, const ApplyingRow&) = default;
};

// The share of `applying` done, 0 to 1: by bytes, or by items when they have no size.
[[nodiscard]] double applied_share(const ApplyingRow& applying);
// "Applying <title>: N of M items, X of Y - <current item>".
[[nodiscard]] std::string applying_text(const ApplyingRow& applying);
// How long `applying` has run at `now`: "45 s", "12 min 5 s", "1 h 3 min".
[[nodiscard]] std::string applying_elapsed(const ApplyingRow& applying, WallTime now);

// cleaning.status.
struct CleaningStatus {
  bool scanning = false;
  double progress = 0.0;  // 0 to 1 while scanning
  std::optional<WallTime> last_completed;
  std::int64_t candidates = 0;
  Bytes bytes;
  std::optional<WallTime> next_auto_run;
  std::vector<std::int64_t> purging;    // quarantine runs being deleted now or queued for it
  std::optional<ApplyingRow> applying;  // nullopt when nothing runs, or from an older service
  std::string unavailable;  // why scans or executions cannot run now; empty when they can

  friend bool operator==(const CleaningStatus&, const CleaningStatus&) = default;
};

// One plan of plans.list (PlanSummary).
struct PlanRow {
  std::int64_t id = 0;
  std::string title;
  std::string kind;   // cleanup, handler, native, relocate, system_setting, quarantine_restore,
                      // recycle_bin, archive, uninstall
  std::string tier;   // auto, ask, report
  std::string scope;  // user (run by the tray) or service
  std::string state;  // core/model/plan.hpp text forms: awaiting_approval, applied, ...
  std::string proposer;
  WallTime created_at;
  std::int64_t items = 0;
  Bytes bytes;
  bool needs_restart = false;
  // The newest re-measurement of the plan's metric: pending, better, unchanged, worse, error;
  // empty before it was applied (or from a service older than this field).
  std::string verification;
};

// One item of plans.get. `approved` is nullopt while the owner has not decided.
struct PlanItemRow {
  std::int64_t id = 0;
  std::string path;  // a file system path, or "handler:<name>", "relocate:<cache>", ...
  Bytes bytes;
  std::optional<std::string> rule;
  std::string reason;
  std::optional<bool> approved;
};

// plans.get: the plan with its items, the exact commands it runs and how it is undone.
struct PlanDetail {
  PlanRow plan;                    // plan.items is the total item count
  std::vector<PlanItemRow> items;  // the first ipc::max_plan_items
  std::vector<std::string> commands;
  std::string undo;
  std::vector<std::string> warnings;
  std::optional<Bytes> freed;  // once applied
  std::string outcome;         // why it failed, was rejected or rolled back; empty otherwise
};

// The items of one quarantine run (quarantine.list), held until `purge_after`.
struct QuarantineItemRow {
  std::int64_t item = 0;
  std::string original;  // where it came from and returns to on restore
  Bytes bytes;
};
struct QuarantineRun {
  std::int64_t plan = 0;
  WallTime moved_at;
  WallTime purge_after;
  Bytes bytes;
  std::vector<QuarantineItemRow> items;
};

// The Space lists (space spec), read-only. `id` is the candidate id that plans.propose_cleanup
// takes.
struct TopFileRow {
  std::int64_t id = 0;
  std::string path;
  Bytes size_on_disk;
  WallTime modified;
  std::string protected_by;  // the deny-list entry that refuses it; empty when it can be proposed
};
struct TopFolderRow {
  std::int64_t id = 0;  // the folder id space.top takes to look inside; 0 from an older service
  std::string path;
  Bytes size_on_disk;
  std::uint64_t files = 0;
};
struct OldDownloadRow {
  std::int64_t id = 0;
  std::string path;
  Bytes size_on_disk;
  std::int64_t age_days = 0;
  std::optional<std::string> origin;  // the HostUrl (or referrer) of Zone.Identifier
};
struct DuplicateFileRow {
  std::int64_t id = 0;
  std::string path;
  WallTime modified;
};
struct DuplicateGroupRow {
  Bytes size;  // of each file
  Bytes reclaimable;
  std::vector<DuplicateFileRow> files;
};
struct UsedInstallerRow {
  std::int64_t id = 0;
  std::string path;
  std::string kind;  // msi or exe
  Bytes size_on_disk;
  std::string installed_as;
};

// A large file of the owner's folders that nothing has written for half a year
// (space.forgotten_files): a report the owner may act on.
struct ForgottenFileRow {
  std::int64_t id = 0;
  std::string path;
  Bytes size_on_disk;
  WallTime modified;
  std::int64_t idle_days = 0;
};

// The owner decisions (space.owner_decisions); `id` is the candidate id plans.owner_action takes.
// A cold folder of the owner's profile, with the archive folder it would move to.
struct ColdFolderRow {
  std::int64_t id = 0;
  std::string path;
  Bytes bytes;
  std::string target;        // empty when no healthy HDD keeps 10 % free after taking it
  std::string reason;        // since when nothing changed in it
  std::string protected_by;  // the deny-list entry that refuses it; empty when it can move
};
// An installed program in an area the knowledge packs mark for uninstall review.
struct ProgramRow {
  std::int64_t id = 0;
  std::string program;
  std::string publisher;
  std::string version;
  std::string location;
  Bytes bytes;
  std::string area;
  std::string reason;
};

// The copy to keep: the oldest, most likely the original.
[[nodiscard]] const DuplicateFileRow* keep_hint(const DuplicateGroupRow& group);

// Why a largest file is not proposed, from its deny-list entry (TopFileRow::protected_by).
[[nodiscard]] std::string_view protected_text(std::string_view rule);

// What the owner can ask for in the Space view.
enum class SpaceList {
  top,
  old_downloads,
  duplicates,
  used_installers,
  owner_decisions,
  forgotten_files
};

// One answer of a space method, or why the call failed.
struct SpaceReply {
  SpaceList list = SpaceList::top;
  std::string volume;  // space.top: the volume asked for, "C:"
  // space.top: the id of the folder looked inside; nullopt for the volume itself.
  std::optional<std::int64_t> folder;
  std::string state;  // ready, scanning (the previous result, if any, is shown) or failed
  std::optional<WallTime> scanned_at;
  std::string error;  // why the call or the analysis failed; empty otherwise
  std::vector<TopFileRow> top_files;
  std::vector<TopFolderRow> top_folders;   // the largest at any depth
  std::vector<TopFolderRow> top_children;  // the largest directly inside top_path
  std::string top_path;                    // the volume root or folder analysed
  Bytes top_size;                          // of top_path and everything below it
  Bytes top_own_size;                      // of the files directly inside top_path
  std::vector<OldDownloadRow> downloads;
  std::vector<DuplicateGroupRow> duplicates;
  std::vector<UsedInstallerRow> installers;
  std::vector<ForgottenFileRow> forgotten;
  Bytes forgotten_min_size;              // space.forgotten_files: the size a file has at least
  std::int64_t forgotten_idle_days = 0;  // and the days nothing has written it
  std::vector<ColdFolderRow> cold;
  std::vector<ProgramRow> programs;
  std::vector<std::string> notes;  // owner decisions: what could not be judged, and why
};

[[nodiscard]] Result<CleaningStatus> parse_cleaning_status(const nlohmann::json& reply);

// plans.list of every state.
[[nodiscard]] Result<std::vector<PlanRow>> parse_plans_list(const nlohmann::json& reply);

[[nodiscard]] nlohmann::json plan_id_params(std::int64_t id);  // plans.get, reject, undo
[[nodiscard]] Result<PlanDetail> parse_plans_get(const nlohmann::json& reply);

// The owner's choice of items of a pending plan, as plans.approve takes it.
struct Selection {
  bool all = false;
  std::vector<std::int64_t> items;   // approved, when not all
  std::vector<std::int64_t> except;  // not approved, when all
};
// Every item of the plan.
[[nodiscard]] Selection select_all();
// The ticked items of `detail`: by their ids when there are at most ipc::max_record_elements of
// them, else as all but the unticked ones when those are few enough and every item of the plan
// is listed. Fails when neither fits, or when nothing is ticked.
[[nodiscard]] Result<Selection> select_items(const PlanDetail& detail,
                                             const std::set<std::int64_t>& ticked);
[[nodiscard]] nlohmann::json plans_approve_params(std::int64_t id, const Selection& selection);
// plans.propose_cleanup of Space candidates (the tray proposes as the owner).
[[nodiscard]] nlohmann::json propose_cleanup_params(const std::vector<std::int64_t>& ids);
// plans.owner_action of one owner-decision candidate: the owner's click is the approval.
[[nodiscard]] nlohmann::json owner_action_params(std::int64_t candidate);

[[nodiscard]] Result<std::vector<QuarantineRun>> parse_quarantine_list(const nlohmann::json& reply);
// quarantine.restore of every held item of a run, or of one item.
[[nodiscard]] nlohmann::json quarantine_restore_params(std::int64_t plan,
                                                       std::optional<std::int64_t> item);

[[nodiscard]] std::string_view space_method(SpaceList list);
// space.top takes the volume as a drive letter ("C" for "C:"), or the id of a folder of an earlier
// answer to look inside; `refresh` starts a new analysis.
[[nodiscard]] nlohmann::json space_params(SpaceList list, std::string_view volume, bool refresh,
                                          std::optional<std::int64_t> folder = std::nullopt);
// Fills `reply` (whose list is set) from the method's answer.
[[nodiscard]] Result<void> parse_space(const nlohmann::json& answer, SpaceReply& reply);

// Inbox views over plans.list.
inline constexpr std::string_view awaiting_approval = "awaiting_approval";

[[nodiscard]] bool pending(const PlanRow& plan);
[[nodiscard]] int pending_count(const std::vector<PlanRow>& plans);
// Whether the owner may ask to undo `plan`: it was applied, and it is a relocation, a system
// setting, a quarantine (an `ask` cleanup) or a folder moved to the HDD archive. Deletions, native
// commands and handlers regenerate and cannot be undone. The service decides; this only offers the
// button.
[[nodiscard]] bool undoable(const PlanRow& plan);
// The state of `plan` as the inbox shows it: "applied, verification pending" while its metric is
// still to be measured again (it is not counted as verified), its state otherwise.
[[nodiscard]] std::string state_text(const PlanRow& plan);
// The bytes of the pending plans per tier, and per kind, in order of first appearance.
using ByteTotals = std::vector<std::pair<std::string, Bytes>>;
[[nodiscard]] ByteTotals pending_bytes_by_tier(const std::vector<PlanRow>& plans);
[[nodiscard]] ByteTotals pending_bytes_by_kind(const std::vector<PlanRow>& plans);

// What one poll learned for the inbox. A member that is nullopt was not asked for, or its method
// failed (a service that does not offer it yet).
struct CleanupPoll {
  std::optional<CleaningStatus> status;
  std::optional<std::vector<PlanRow>> plans;
  std::vector<PlanDetail> details;  // of the plans the owner opened
  std::optional<std::vector<QuarantineRun>> quarantine;
  // Why the owner's last inbox request failed; empty when it worked.
  std::optional<std::string> action;
  std::vector<SpaceReply> space;
  // The user-task runner: what runs now (empty when nothing), and how the last task ended.
  std::optional<std::string> task_activity;
  std::optional<std::string> task_note;
};

// The approval inbox and the Space view, built from poll results. Not thread-safe.
class CleanupView {
 public:
  // Takes in one poll. The details of plans that left the list are dropped.
  void apply(CleanupPoll poll);

  [[nodiscard]] const std::optional<CleaningStatus>& status() const { return status_; }
  // Every plan, newest first; nullopt before the first.
  [[nodiscard]] const std::optional<std::vector<PlanRow>>& plans() const { return plans_; }
  [[nodiscard]] int pending() const { return plans_ ? pending_count(*plans_) : 0; }
  // The details of plan `id`; null until they arrived.
  [[nodiscard]] const PlanDetail* detail(std::int64_t id) const;
  [[nodiscard]] const std::optional<std::vector<QuarantineRun>>& quarantine() const {
    return quarantine_;
  }
  [[nodiscard]] const std::string& action_error() const { return action_error_; }
  // The newest answer of `list`; nullopt before the owner asked for it.
  [[nodiscard]] const std::optional<SpaceReply>& space(SpaceList list) const {
    return space_.at(static_cast<std::size_t>(list));
  }
  [[nodiscard]] const std::string& task_activity() const { return task_activity_; }
  [[nodiscard]] const std::string& task_note() const { return task_note_; }

 private:
  std::optional<CleaningStatus> status_;
  std::optional<std::vector<PlanRow>> plans_;
  std::vector<PlanDetail> details_;
  std::optional<std::vector<QuarantineRun>> quarantine_;
  std::string action_error_;
  std::array<std::optional<SpaceReply>, 6> space_;
  std::string task_activity_;
  std::string task_note_;
};

}  // namespace myclean::tray_model
