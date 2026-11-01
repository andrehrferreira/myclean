#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/drives/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/model/result.hpp"
#include "core/scanall/findings.hpp"

// Where scan-all takes each drive's health from. A drive whose health is not good is never the
// target of an archive or relocate finding (scan-all spec, "Cold Data" and "Placement Advice").
namespace myclean::scanall {

// The service's disks.health reply (ipc/disk_health.hpp) as a HealthView: each disk's number,
// model, bus, source, status ("failing" is critical) and reasons. Fails on a reply of another
// shape.
[[nodiscard]] Result<health::HealthView> parse_service_health(const nlohmann::json& reply);

struct ChosenHealth {
  health::HealthView view;
  std::string note;  // where the health came from, for the report's notes
};

// The service's health when `service` (its disks.health reply, or why it could not be called)
// holds the reading of at least one disk: the service runs as SYSTEM and reads every disk's
// SMART data or NVMe health log. Otherwise `local()`, what this process can read itself, with a
// note saying why the service's was not used and which disks are then of unknown health.
[[nodiscard]] ChosenHealth choose_health(const Result<nlohmann::json>& service,
                                         const std::function<health::HealthView()>& local);

// The scan's drives: each volume of `states` with the worst health of its disks in `view`
// (unknown when it has no disk or a disk has no reading), `system` for the one of `system_root`.
[[nodiscard]] std::vector<DriveInfo> drive_infos(std::span<const drives::DriveState> states,
                                                 const health::HealthView& view,
                                                 const std::filesystem::path& system_root);

}  // namespace myclean::scanall
