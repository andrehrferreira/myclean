#pragma once

#include <string_view>

// The resident-process methods (optimizer spec, "Memory-Leak Detection", "Background-App Cost
// Report"; core/resident). A program is the lower-case file name of the root image of its
// process trees ("image"); "key" is its token form (entity_token), the entity of its
// process.memory-leak alert and the parameter of resident.trend. Times are Unix seconds (UTC).
// Both methods only read: nothing here acts on a process.
namespace myclean::ipc {

// No parameters. Every resident program of the last 30 days (one that ran at least 24 hours of
// them, counted in minute samples), most CPU time first, from the hours stored so far (the
// current hour is not in it yet):
// {"days": 30, "from": the start of the range,
//  "programs": [{"image", "key", "hours_running": number, "cpu_seconds", "ram_average",
//                "ram_peak", "disk_bytes", "net_bytes"}]}
// RAM is the private working set of every process of the program's trees, sampled once a
// minute ("ram_peak" is the largest sample, "ram_average" the average over the minutes it ran);
// disk bytes are I/O transfers as Windows counts them (read, write and other); network bytes are
// sent plus received.
inline constexpr std::string_view resident_costs_method = "resident.costs";
inline constexpr int resident_costs_days = 30;
inline constexpr int resident_min_hours = 24;

// Parameters: key (token, required). The private-bytes trend of one program over the last 48
// hours, for the leak alert's chart:
// {"image", "key", "points": [{"time": the 5-minute bucket's start, "private_bytes": its
//  average}], "leak": null | {"hours": whole hours of steady growth, "hourly": [the median
//  private bytes of each of those hours, oldest first]}}
// Fails with not_found when no program has that key in the last 48 hours.
inline constexpr std::string_view resident_trend_method = "resident.trend";

}  // namespace myclean::ipc
