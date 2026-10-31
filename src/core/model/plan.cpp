#include "core/model/plan.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean {

namespace {

constexpr std::array<std::string_view, 10> plan_state_names = {
    "proposed", "auto",     "awaiting_approval", "approved", "applying",
    "applied",  "verified", "rolled_back",       "failed",   "rejected",
};

constexpr std::array<std::string_view, 3> item_approval_names = {"pending", "approved",
                                                                 "not_approved"};

}  // namespace

std::string_view to_string(PlanState state) {
  return detail::enum_name(plan_state_names, state);
}

std::optional<PlanState> parse_plan_state(std::string_view text) {
  return detail::enum_from_name<PlanState>(plan_state_names, text);
}

std::string_view to_string(ItemApproval approval) {
  return detail::enum_name(item_approval_names, approval);
}

std::optional<ItemApproval> parse_item_approval(std::string_view text) {
  return detail::enum_from_name<ItemApproval>(item_approval_names, text);
}

}  // namespace myclean
