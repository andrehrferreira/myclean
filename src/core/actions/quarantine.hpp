#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {

// Quarantine layout: <volume>\.myclean-quarantine\<run>\<item>\<original name>, where <run> is
// the id of the plan that quarantined the item and <item> its plan item id, so two items with
// the same name never collide. The move is a rename on the item's own volume: the item keeps
// its identity, and nothing is copied.
inline constexpr std::string_view quarantine_folder = ".myclean-quarantine";

// Where `original`, item `item` of run `run`, is kept while quarantined.
[[nodiscard]] std::filesystem::path quarantine_location(const std::filesystem::path& original,
                                                        PlanId run, PlanItemId item);

enum class QuarantineState { held, restored, purged };

[[nodiscard]] std::string_view to_string(QuarantineState state);
[[nodiscard]] std::optional<QuarantineState> parse_quarantine_state(std::string_view text);

// One row of `quarantine`.
struct QuarantineRecord {
  std::int64_t id = 0;
  PlanId run;
  PlanItemId item;
  std::filesystem::path original;
  std::filesystem::path location;
  FileIdentity identity;  // unchanged by the move: re-checked before restore or purge
  Bytes size;
  WallTime moved_at;
  WallTime purge_after;
  QuarantineState state = QuarantineState::held;
};

// Stores `record` and returns its id; record.id is ignored.
[[nodiscard]] Result<std::int64_t> add_quarantine(store::Database& db,
                                                  const QuarantineRecord& record);
[[nodiscard]] Result<void> set_quarantine_state(store::Database& db, std::int64_t id,
                                                QuarantineState state);
// Removes a record written ahead of a move that did not happen.
[[nodiscard]] Result<void> remove_quarantine(store::Database& db, std::int64_t id);
// The records of run `run`, in item order.
[[nodiscard]] Result<std::vector<QuarantineRecord>> quarantine_records(store::Database& db,
                                                                       PlanId run);
// Every record still held, of any run.
[[nodiscard]] Result<std::vector<QuarantineRecord>> held_quarantine(store::Database& db);

// Restore and purge of quarantined items. Both build their own stored plan (actions
// "RestoreQuarantine" and "PurgeQuarantine"), so the moves and deletes go through SafeRemove,
// with every outcome in action_log.
class Quarantine {
 public:
  static constexpr std::chrono::days hold_time{7};

  // `environment` and `engine` must outlive the quarantine.
  Quarantine(const Environment& environment, PolicyEngine& engine)
      : environment_(environment), engine_(&engine) {}

  // Moves the held items `items` of run `run` (every held item when empty) back to their
  // original paths. Fails with io_error, naming the count, when some item stays in quarantine,
  // and with refused for an `mcp:*` actor.
  [[nodiscard]] Result<void> restore(PlanId run, std::span<const PlanItemId> items,
                                     const Actor& actor);

  // Deletes the quarantine folder of every run whose items all passed their expiry. Returns how
  // many records were purged. Fails with refused for an `mcp:*` actor.
  [[nodiscard]] Result<std::size_t> purge(const Actor& actor);

 private:
  // Stores `plan` for `actor` and moves it to applying.
  [[nodiscard]] Result<Plan> start(Plan plan, const Actor& actor);
  // Commits `entries` and the new `state` of `records`, then moves `plan` to applied, or to
  // failed when `kept` items stayed where they were.
  [[nodiscard]] Result<void> finish(PlanId plan, std::span<const LogEntry> entries,
                                    std::span<const std::int64_t> records, QuarantineState state,
                                    std::size_t kept, const Actor& actor);

  Environment environment_;
  PolicyEngine* engine_;
};

// Moves the items of one stored plan into quarantine through SafeRemove (tier ask), recording
// each in `quarantine` before the move with an expiry 7 days later. Undo restores them.
class QuarantineItems final : public ItemAction {
 public:
  // `quarantine` must outlive the action.
  QuarantineItems(const Environment& environment, Quarantine& quarantine, PlanId plan, Actor actor);

  [[nodiscard]] ActionInfo describe() const override;
  // Undo holds, for every moved item, its quarantine location.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Restores the items of `undo` to their original paths.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  Quarantine* quarantine_;
};

}  // namespace myclean::actions
