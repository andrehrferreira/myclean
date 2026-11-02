#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/map/attribution.hpp"
#include "core/rules/schema.hpp"
#include "core/space/scanner.hpp"

namespace myclean::map {

// Areas for the developer artifacts in `scan`, which no fixed location can name: in every folder
// holding the marker of a project rule, what the rule's targets select there (only folders
// holding require.contains, when the rule sets it; never files). One area per rule, with exact
// paths, in rule order. Like the artifact scanner, a folder named in `skip` and everything below
// an artifact is not searched for projects. The areas explain bytes only: whether the rule
// proposes an artifact depends on git, use and inactivity. A path holding '%' is left out (it
// would read as a variable).
[[nodiscard]] std::vector<Area> project_areas(const space::ScanResult& scan,
                                              std::span<const rules::CleaningRule> rules,
                                              std::span<const std::wstring> skip);

}  // namespace myclean::map
