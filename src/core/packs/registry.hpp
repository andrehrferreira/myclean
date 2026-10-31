#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/map/attribution.hpp"
#include "core/packs/applies.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/schema.hpp"

// The content of the packs in effect, per case kind, in the form the engines consume: the clean
// rules for clean::RuleEngine (as a rules::RuleCatalog lists them), the areas for the disk map,
// the knowledge base for trust, the classes for the governor, and the relocation recipes and
// compression targets for placement and compress. Every id is "<pack>/<case>".
namespace myclean::packs {

struct Registry {
  std::vector<std::string> packs;  // the ids of the packs included, in order
  std::vector<rules::CleaningRule> clean;
  std::vector<map::Area> attribution;
  std::vector<RelocationRecipe> relocate;
  std::vector<CompressTarget> compress;
  std::vector<rules::KbEntry> kb;
  rules::ClassSet classes;  // matches in pack order (first match wins); default job caps
};

// Every pack's content, in the order of `packs` (load_packs' precedence order).
[[nodiscard]] Registry make_registry(std::span<const Pack> packs);

// The content of the packs whose applies_when holds on this machine.
[[nodiscard]] Registry make_registry(std::span<const Pack> packs, const Probes& probes);

}  // namespace myclean::packs
