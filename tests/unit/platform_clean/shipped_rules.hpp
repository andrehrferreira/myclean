#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/rules/schema.hpp"

namespace myclean::platform::testing {

// The clean cases of every pack shipped in data/packs (applies_when aside); empty when a pack is
// rejected.
inline std::vector<rules::CleaningRule> shipped_rules() {
  const packs::LoadReport report =
      packs::load_packs(packs::DiskPackFiles{}, std::filesystem::path{MYCLEAN_DATA_DIR} / "packs",
                        std::nullopt, packs::this_version());
  return report.errors.empty() ? packs::make_registry(report.packs).clean
                               : std::vector<rules::CleaningRule>{};
}

}  // namespace myclean::platform::testing