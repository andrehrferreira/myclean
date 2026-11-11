#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"

// The monthly snapshot (reports spec, "Monthly Snapshot"): the state the monthly report keeps and
// compares, read through the service's own read methods (the health check, the storage analysis
// and the intruder scan of prompts 1, 3 and 5) and the stored history. Taking it changes nothing:
// it calls only read methods and reads the store.
namespace myclean::report {

// Calls one read method of the service in-process, as a pipe client would.
using Call =
    std::function<Result<nlohmann::json>(std::string_view method, const nlohmann::json& params)>;
// Waits `delay` before a running analysis is asked again; false when the job must stop.
using Wait = std::function<bool(std::chrono::seconds delay)>;

// Every category of a snapshot, in report order. Each is {"status": "collected", ...data} or
// {"status": "not_collected", "reason": why}, never missing and never silently empty:
//   startup_items       {"items": [{"key", "name", "source", "scope", "command", "enabled"}]}
//                       (startup.list without its services)
//   browser_extensions  {"items": [{"key": "<browser>/<profile>/<id>", "browser", "profile",
//                        "id", "name", "version", "origin", "enabled", "flag"}]}
//   services            {"items": [{"key", "name", "display_name", "command", "signer"}]} (the
//                       auto-start services not published by Microsoft Windows, startup.list)
//   listening_ports     {"items": [{"key": "<protocol>/<port>/<image>", "protocol", "port",
//                        "image", "pid"}]}
//   unsigned_binaries   {"items": [{"key": lower-case path, "path", "status": "unsigned"|
//                        "invalid", "seen_as": ["process"|"persistence"|"network"]}]} (process
//                       images, persistence targets, which include the services, and network
//                       talkers whose signature is missing or invalid)
//   top_folders         {"volume", "folders": [{"path", "size_on_disk", "files"}],
//                        "files": [{"path", "size_on_disk"}]} (space.top of the system volume)
//   cleanup_candidates  {"old_downloads"|"duplicate_photos"|"used_installers": {"count", "bytes"}
//                        | {"error"}}: listed only, never acted on
//   disk_health         {"drives": [{"key", "model", "status", "reasons", "temperature_c",
//                        "percent_used", "available_spare", "media_errors",
//                        "reallocated_sectors", "pending_sectors", "power_on_hours",
//                        "data_written_bytes"}]}
//   free_space          {"volume", "size", "free", "points": [{"time", "free"}], "change"}
//                       (daily means of volume.free over the last 30 days)
//   health              {"days": 30, "cpu": {"mean", "max"}|null, "memory_available":
//                        {"mean", "min"}|null, "programs": [{"image", "hours_running",
//                        "cpu_percent", "ram_average", "ram_peak"}]} (30-day aggregates of the
//                       history, never one sample: the resident programs of resident.costs, the
//                       most CPU first, CPU as a share of all logical processors while running)
//   security            {"counts", "suspicious": [{"name", "source", "summary"}],
//                        "known_dangerous": [{"name", "source", "summary"}],
//                        "defender_detections", "flagged_extensions",
//                        "background_talkers": [{"image", "sent", "received"}]}
//   governor            {"interventions", "applied", "recorded_only", "by_step": {step: count}}
//   freezes             {"incidents", "longest_ms": int|null}
//   space_freed         {"bytes", "items"} (what the automatic cleaning deleted)
// Counts of the last three cover the snapshot's period.
inline constexpr std::array<std::string_view, 14> categories{
    "startup_items", "browser_extensions", "services",    "listening_ports", "unsigned_binaries",
    "top_folders",   "cleanup_candidates", "disk_health", "free_space",      "health",
    "security",      "governor",           "freezes",     "space_freed"};

// The categories compared item by item ("key").
inline constexpr std::array<std::string_view, 5> set_categories{
    "startup_items", "browser_extensions", "services", "listening_ports", "unsigned_binaries"};

inline constexpr std::chrono::days health_days{30};

struct Probes {
  Call call;
  Wait wait;
  // governor_event, input_incident and action_log; null: those categories are not collected.
  store::Store* store = nullptr;
  std::chrono::seconds poll_interval{15};
  // A storage analysis still running after this many polls is not collected.
  int max_polls = 240;
};

// The snapshot taken at `now` of the local month it falls in:
// {"month": "YYYY-MM", "taken_at", "period": {"from", "to"}, "categories": {name: category}}
// Governor interventions, freeze incidents and space freed count [period_start, now). A
// category whose source fails, or that the job stopped before, is not collected with the reason.
[[nodiscard]] nlohmann::json take_snapshot(const Probes& probes, WallTime now,
                                           WallTime period_start,
                                           const std::chrono::time_zone& zone);

// A category of `snapshot`, or a not-collected one when it is missing.
[[nodiscard]] nlohmann::json category(const nlohmann::json& snapshot, std::string_view name);
[[nodiscard]] bool collected(const nlohmann::json& category);

}  // namespace myclean::report
