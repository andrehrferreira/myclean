#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/clean/engine.hpp"
#include "core/compress/analyzer.hpp"
#include "core/drives/analysis.hpp"
#include "core/map/disk_map.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "core/scanall/findings.hpp"

// The findings of the existing analyses: the disk map, the cleaning rules, the compression
// analyzer and the drive maintenance analysis (scan-all spec, "Unified Report").
namespace myclean::scanall {

// The map's places of at least `min_size` on `drive`, by their reclaim kind:
//   clean-rule  a delete or quarantine rule: nothing (the rule engine's own candidates are the
//               items); a native or handler rule: remove with the rule's command or handler; a
//               rule that is not loaded: manual-review
//   handler     remove with cleanup_handler
//   setting     hibernation: remove, half of it (reduced keeps Fast Startup); shadow_storage:
//               remove; page_file: relocate all but 4 GiB to the best healthy drive
//   uninstall   uninstall-review
//   manual-review  manual-review
//   relocate, none  nothing (relocation is the placement advisor's)
// and every folder the map leaves unexplained, as manual-review. Plans that come from a rule
// carry its id as parameter "rule".
[[nodiscard]] std::vector<Finding> map_findings(const map::DiskMap& map, const DriveInfo& drive,
                                                std::span<const rules::CleaningRule> rules,
                                                std::span<const DriveInfo> drives, Bytes min_size);

// The rule engine's candidates (`source`: "rules" for evaluate, "artifacts" for scan_artifacts):
// remove (manual-review for tier report) with the plan of the rule's strategy. A native or
// handler candidate names no path: it is put on the system drive, and left out when an
// `earlier` finding already carries its rule or handler.
[[nodiscard]] std::vector<Finding> rule_findings(const clean::Evaluation& evaluation,
                                                 std::string_view source,
                                                 std::span<const Finding> earlier,
                                                 std::span<const DriveInfo> drives);

// A compress finding for the recommended algorithm of `estimate`; nullopt without one.
[[nodiscard]] std::optional<Finding> compress_finding(const compress::FolderEstimate& estimate,
                                                      const DriveInfo& drive);

// defrag-trim for a recommended retrim or defragmentation; manual-review when the drive needs
// one but its health keeps it from being recommended; nothing otherwise.
[[nodiscard]] std::optional<Finding> maintenance_finding(const drives::VolumeAnalysis& analysis);

}  // namespace myclean::scanall
