#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/store.hpp"

namespace myclean::actions {

// What the file actions work with. Every pointer must outlive the actions using it.
struct Environment {
  store::Store* store = nullptr;                 // opened with action_migrations()
  const os::IFileSystem* file_system = nullptr;  // read only: every change goes through SafeRemove
  safety::SafeRemove* safe_remove = nullptr;     // over a StorePlanSource of `store`
  const os::IClock* clock = nullptr;
};

// Size on disk of `path` and, for a folder, everything inside it; reparse points are counted
// as themselves and never entered. Zero when `path` does not exist.
[[nodiscard]] Result<Bytes> measure(const os::IFileSystem& file_system,
                                    const std::filesystem::path& path);

// The part DeleteItems and QuarantineItems share: an action over the file system items of one
// stored plan, run by one actor.
class ItemAction : public Action {
 public:
  // The plan's items the owner did not reject, as stored.
  [[nodiscard]] Result<Preview> preview() override;
  // Metric "bytes_at_original_paths": before is the stored size of each item in `undo`, after
  // what is on disk at its path now. Less is better.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;

 protected:
  ItemAction(const Environment& environment, PlanId plan, Actor actor);

  // The items of `preview` to hand to SafeRemove, and outcomes for those refused here: an item
  // whose path or identity differs from the stored plan item with its id is refused with
  // "preview_mismatch", so apply never acts on something the preview did not show. Items the
  // plan does not contain are passed on: SafeRemove refuses them with "not_in_plan".
  struct Selection {
    Plan plan;  // as stored
    std::vector<PlanItem> items;
    std::vector<safety::ItemOutcome> refused;
  };
  [[nodiscard]] Result<Selection> select(const Preview& preview) const;

  // The action_log entry of one SafeRemove outcome. `requested` is the path the request
  // named, recorded also when the item is not in the plan.
  [[nodiscard]] LogEntry outcome_entry(std::string_view event, const safety::ItemOutcome& outcome,
                                       const std::filesystem::path& requested) const;

  [[nodiscard]] const Environment& environment() const { return environment_; }
  [[nodiscard]] PlanId plan_id() const { return plan_; }
  [[nodiscard]] const Actor& actor() const { return actor_; }

 private:
  [[nodiscard]] Result<Plan> load_plan() const;

  Environment environment_;
  PlanId plan_;
  Actor actor_;
};

}  // namespace myclean::actions
