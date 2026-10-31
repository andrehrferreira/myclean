#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/units.hpp"

namespace myclean {

// The policy states of a plan (docs/architecture.md section 6). The legal transitions are
// enforced by the policy state machine, not here.
enum class PlanState {
  proposed,
  automatic,  // text form "auto": the feature's automation level skips owner approval
  awaiting_approval,
  approved,
  applying,
  applied,
  verified,
  rolled_back,
  failed,
  rejected,  // the owner approved none of the items
};

[[nodiscard]] std::string_view to_string(PlanState state);
[[nodiscard]] std::optional<PlanState> parse_plan_state(std::string_view text);

// The owner's decision on one item; only approved items are applied.
enum class ItemApproval { pending, approved, not_approved };

[[nodiscard]] std::string_view to_string(ItemApproval approval);
[[nodiscard]] std::optional<ItemApproval> parse_item_approval(std::string_view text);

// One file system entry a plan acts on.
struct PlanItem {
  PlanItemId id;
  std::filesystem::path path;
  FileIdentity identity;  // recorded when the plan was built; re-checked before acting
  Bytes size;             // size on disk: what removing the item frees
  ItemApproval approval = ItemApproval::pending;
};

// A list of items one action will change, produced from a finding or a request.
struct Plan {
  PlanId id;
  std::string action;  // the action that executes it, e.g. "DeleteItems"
  std::optional<FindingId> finding;
  Actor proposer;
  PlanState state = PlanState::proposed;
  WallTime created_at;
  std::vector<PlanItem> items;
};

}  // namespace myclean
