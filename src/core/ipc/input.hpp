#pragma once

#include <cstddef>
#include <string_view>

// The input-watchdog methods of the service (input-watchdog spec). The watchdog runs in the
// tray, in the owner's session; the service stores its incidents in input_incident.
namespace myclean::ipc {

// Parameters: incident (record, required): one incident in the record form of
// core/input/incident_record.hpp (input::to_record), checked against that strict schema before
// anything runs. Stores it and answers {"id"}. The record is data: none of its strings is ever
// used as a path, command or any other OS input.
inline constexpr std::string_view input_record_incident_method = "input.record_incident";

// No parameters. Every stored incident, classified and grouped by mechanism and process (the
// Freezes view), groups ordered by their latest incident, newest first:
// {"groups": [{"mechanism": "delayed_rendering"|"clipboard_held_open"|"viewer_chain"|
//              "slow_keyboard_hook"|"heavy_payload"|"unexplained",
//              "process": image name|null, "count", "latest": Unix seconds,
//              "incidents": [{"id", the members of the incident record, "cause": {"pid",
//                             "image"}|null, "evidence": [text]}]}]}
// "incidents" holds the newest max_summary_incidents of the group; "count" counts all. An empty
// "groups" means no freeze over 250 ms was recorded.
inline constexpr std::string_view input_summary_method = "input.summary";
inline constexpr std::size_t max_summary_incidents = 20;

}  // namespace myclean::ipc
