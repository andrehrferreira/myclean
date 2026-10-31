#pragma once

#include <string_view>

// The disk-health read method of the service (disk-health spec). Times are Unix seconds (UTC).
namespace myclean::ipc {

// No parameters. The newest health reading of every physical disk:
// {"time": the read's time, or null before the first read completes,
//  "drives": [{"key": the drive's stable identity in token form (entity_token of
//              "<model>#<serial hash>"; the entity of its alerts in alerts.list),
//              "disk": the current disk number ("3"), "model", "bus": "nvme"|"ata"|"usb"|"other",
//              "source": "nvme_health_log"|"ata_smart"|"sat_pass_through"|
//                        "reliability_counters"|"none",
//              "volumes": [mount entities on the disk, as in volumes.list ("C:")],
//              "status": "good"|"warning"|"failing"|"unknown", "reasons": [text],
//              "readable": bool (the drive's own health data was read: NVMe log, SMART or SAT),
//              "unreadable_reason": text|null (null when readable, such as "not readable over
//              USB: ..."),
//              "temperature_c", "temperature_limit_c" (null: no rated limit known),
//              "percent_used", "available_spare", "media_errors", "reallocated_sectors",
//              "pending_sectors", "power_on_hours", "data_written_bytes": integer|null (null
//              when the drive does not report it),
//              "endurance": {"state": "projected"|"rating_unknown"|"insufficient_history"|
//                            "not_reported", "years_left": number|null,
//                            "tbw_rated": number|null, "tbw_used_percent": number|null,
//                            "note": text (why the projection is unavailable; empty when
//                            projected)}}]}
// Drives are in disk number order; "drives" is empty until the first read completes (it may
// take a minute after the service starts).
inline constexpr std::string_view disks_health_method = "disks.health";

inline constexpr std::string_view drive_good = "good";
inline constexpr std::string_view drive_warning = "warning";
inline constexpr std::string_view drive_failing = "failing";
inline constexpr std::string_view drive_unknown = "unknown";

}  // namespace myclean::ipc
