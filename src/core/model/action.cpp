#include "core/model/action.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"

namespace myclean {

namespace {

constexpr std::array<std::string_view, 3> risk_names = {"low", "medium", "high"};
constexpr std::array<std::string_view, 3> tier_names = {"auto", "ask", "report"};
constexpr std::array<std::string_view, 2> execution_context_names = {"svc", "user"};
constexpr std::array<std::string_view, 3> effect_names = {"better", "unchanged", "worse"};

}  // namespace

std::string_view to_string(Risk risk) {
  return detail::enum_name(risk_names, risk);
}

std::optional<Risk> parse_risk(std::string_view text) {
  return detail::enum_from_name<Risk>(risk_names, text);
}

std::string_view to_string(Tier tier) {
  return detail::enum_name(tier_names, tier);
}

std::optional<Tier> parse_tier(std::string_view text) {
  return detail::enum_from_name<Tier>(tier_names, text);
}

std::string_view to_string(ExecutionContext context) {
  return detail::enum_name(execution_context_names, context);
}

std::optional<ExecutionContext> parse_execution_context(std::string_view text) {
  return detail::enum_from_name<ExecutionContext>(execution_context_names, text);
}

std::string_view to_string(Effect effect) {
  return detail::enum_name(effect_names, effect);
}

std::optional<Effect> parse_effect(std::string_view text) {
  return detail::enum_from_name<Effect>(effect_names, text);
}

Effect lower_is_better(double before, double after, double tolerance) {
  const double margin = before * tolerance;
  if (after < before - margin) {
    return Effect::better;
  }
  return after > before + margin ? Effect::worse : Effect::unchanged;
}

Bytes Preview::total_size() const {
  Bytes total;
  for (const PlanItem& item : items) {
    total += item.size;
  }
  return total;
}

}  // namespace myclean
