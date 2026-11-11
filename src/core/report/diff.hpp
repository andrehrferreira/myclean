#pragma once

#include <nlohmann/json.hpp>

// The diff of two monthly snapshots (reports spec, "Diff Against The Previous Snapshot"): what
// was added and removed in each item category, how the top folders grew, how each drive's health
// changed, the free-space change of the system volume and the monthly counts before and after.
namespace myclean::report {

// The changes from snapshot `before` to snapshot `after` (take_snapshot answers), in the shape
// of the Diff of core/ipc/reports.hpp. A category not collected in either snapshot is
// {"not_compared": why} instead of a change.
[[nodiscard]] nlohmann::json diff(const nlohmann::json& before, const nlohmann::json& after);

}  // namespace myclean::report
