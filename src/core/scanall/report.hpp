#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/units.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"

// The unified scan-all report (scan-all spec, "Unified Report" and "No Double Counting").
namespace myclean::scanall {

struct DriveSummary {
  DriveInfo drive;
  std::array<Bytes, action_group_count> counted{};  // by ActionGroup
};

struct Report {
  WallTime time;
  std::vector<DriveSummary> drives;
  // By group (ActionGroup order), urgent first, then most counted bytes first.
  std::vector<Finding> findings;
  std::array<Bytes, action_group_count> totals{};  // counted bytes of the machine, by group
  std::vector<std::string> notes;
};

// Builds the report from every finding of every drive:
//   1. Health: on a drive whose health is not good, a finding that would write there (remove,
//      deduplicate, compress, defrag-trim) becomes manual-review, its reason naming the health.
//   2. Precedence: each byte of a drive's tree (`trees`, matched to drives by root) is owned by
//      one finding: the one whose items cover it with the first group in ActionGroup order
//      (remove > deduplicate > relocate > archive > compress > defrag-trim > uninstall-review >
//      manual-review), and within one group the most precise item (the deepest path; the
//      earlier finding on a tie). A finding counts bytes x min(1, owned / footprint), so a byte
//      counted under remove is never counted again under compress, relocate or anything else.
//      A finding without items, or on a drive without a tree, counts all its bytes; an item the
//      tree does not hold owns its own size.
//   3. Totals per drive and for the machine, by group, from the counted bytes.
[[nodiscard]] Report build_report(std::vector<DriveInfo> drives, std::vector<Finding> findings,
                                  std::span<const space::ScanResult> trees, WallTime time);

// The report as one JSON document: sizes in bytes, paths and text in UTF-8.
[[nodiscard]] nlohmann::json to_json(const Report& report);

}  // namespace myclean::scanall
