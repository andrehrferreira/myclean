#include "core/packs/applies.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

#include "core/clean/patterns.hpp"
#include "core/clean/walk.hpp"
#include "core/packs/fields.hpp"
#include "core/packs/schema.hpp"

namespace myclean::packs {

namespace {

bool same_name(std::string_view one, std::string_view other) {
  return std::ranges::equal(one, other, [](char left, char right) {
    const auto fold = [](char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                  : character;
    };
    return fold(left) == fold(right);
  });
}

bool path_exists(const std::string& pattern, const Probes& probes) {
  const std::optional<std::wstring> expanded =
      clean::expand_variables(detail::from_utf8(pattern).native(), *probes.environment);
  if (!expanded) {
    return false;
  }
  const std::filesystem::path path{*expanded};
  return path.has_root_name() && path.has_root_directory() &&
         !clean::resolve(*probes.tree, path).entries.empty();
}

bool holds(const Condition& condition, const Probes& probes) {
  switch (condition.kind) {
    case ConditionKind::app_installed:
      return std::ranges::any_of(probes.machine->installed_apps(), [&](const std::string& name) {
        return std::regex_search(name, condition.app->regex);
      });
    case ConditionKind::path_exists:
      return path_exists(condition.text, probes);
    case ConditionKind::process_image:
      return std::ranges::any_of(probes.machine->running_images(), [&](const std::string& image) {
        return same_name(image, condition.text);
      });
    case ConditionKind::os_build_min:
      return probes.machine->os_build() >= condition.build;
  }
  return false;
}

}  // namespace

bool applies(const Applicability& applicability, const Probes& probes) {
  const auto holds_here = [&probes](const Condition& condition) {
    return holds(condition, probes);
  };
  return std::ranges::all_of(applicability.all, holds_here) &&
         (applicability.any.empty() || std::ranges::any_of(applicability.any, holds_here));
}

}  // namespace myclean::packs
