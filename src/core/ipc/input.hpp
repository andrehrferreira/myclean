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

// No parameters. Guided freeze elimination (input-watchdog spec I3), for the Freezes view:
//   {"collecting": bool (true while no baseline exists: no incident, or the first is less than 7
//    days old), "baseline": null | Rate (the 7 days before now, earlier pauses left out),
//    "suspects": [{"id": int (what plans.propose_pause takes), "image", "hook": bool (owns a
//    keyboard hook), "mode": "suspend"|"guided", "running": bool|null}] (in suspect rank),
//    "offer": null | {"suspect": int, "image", "mode", "hours": 24} (the highest-ranked running
//    suspect not compared yet, once the baseline has incidents and no pause is active),
//    "active": null | Pause + {"running": bool|null} (the pause now; for a guided pause whether
//    the owner still runs the suspect), "pauses": [Pause] (newest first, at most 50)}
// Rate: {"incidents": int, "hours": number, "per_day": number|null}
// Pause: {"plan": int, "image", "mode": "suspend"|"guided", "started": int, "ends": int,
//         "restored": int|null, "end_reason": null|"period_ended"|"ended_by_owner"|"undone"|
//         "service_restarted"|"service_stopped"|"failed", "baseline": Rate, "during": null|Rate,
//         "change": null|"lower"|"same"|"higher" (the rate during the pause against the
//         baseline)}
// mode suspend: every process of the suspect in the owner's session (never a service, a game,
// an allowed or MyClean process or the foreground app) is suspended for the pause and resumed
// at its end, also by a service restarted after a crash. mode guided (a suspect that owns a
// keyboard hook, which a suspend would turn into system-wide input lag): nothing is suspended;
// the owner quits it, only the hours no process of it runs count, and an alert reminds the owner
// to start it again at the end.
inline constexpr std::string_view input_pauses_method = "input.pauses";

// TRAY ONLY. No parameters. Ends the active pause now (a suspend pause resumes what it
// suspended): {"plan": int, "ended": true}. Fails with not_found when no pause is active.
inline constexpr std::string_view input_end_pause_method = "input.end_pause";

}  // namespace myclean::ipc
