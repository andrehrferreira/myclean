#pragma once

#include <cstddef>
#include <string_view>

// The resource governor's methods (governor spec; core/governing). Times are Unix seconds (UTC).
// A process is named by its PID and its creation time, because PIDs are reused: "created" is the
// creation time in nanoseconds since the Unix epoch (exact for the 100 ns ticks of Windows), and
// a process object is {"pid", "created"}.
//
// An event (a governor_event row):
//   {"id", "time", "kind", "root": process|null, "image", "class": text|null,
//    "step": "priority"|"ecoqos"|"cap_job"|"suspend"|"kill"|null, "reason", "actor", "members",
//    "metrics": {"cpu_percent", "available", "commit", "commit_limit",
//                "hard_faults_per_second"}|null,
//    "detail"}
// "kind" is one of apply_step, roll_back, resume, kill, notify, add_to_allow_list (the ladder's
// commands and the owner's choices), place_job, job_released, job_adopted, job_limits,
// notification_limit, job_memory_limit, commit_failure_exit, untrusted_job, game_detected,
// game_mode, orphan_loops (a pattern of orphaned polling loops proposed, or its trees ended by an
// `always` rule) and orphan_rule (the owner stored or forgot a rule). An orphaned tree that is
// ended is a "kill" event with reason "orphaned_loop". "reason" is a governor::Reason text ("cpu",
// "commit", "pressure_cleared", "resume", "orphaned_suspend", "untrusted_job", "policy",
// "orphaned_loop", ...). "actor" is "governor" or "owner". In
// observe mode "detail" is "observe": the decision was recorded and nothing was changed.
namespace myclean::ipc {

// No parameters. The governor now:
// {"mode": "observe"|"act",
//  "paused_until": integer|null,
//  "pressure": {"state": "normal"|"pressure", "clearing": bool,
//               "reasons": ["cpu"|"available_memory"|"commit"|"hard_faults"],
//               "metrics": {"cpu_percent", "available", "commit", "commit_limit",
//                           "hard_faults_per_second"}|null},
//  "game_mode": {"active": bool, "override": "auto"|"on"|"off", "build_cpu_percent": integer,
//                "games": [{"pid", "created", "image",
//                           "reason": "owner_list"|"library_root"|"game_config_store"|
//                                     "fullscreen_gpu"}]},
//  "jobs": [{"name", "role": "class"|"tree"|"cap", "class", "root": process|null, "root_image",
//            "members": integer, "adopted": bool, "cpu_cap_percent": number|null,
//            "commit_cap": integer|null, "commit_used": integer|null}],
//  "trees": [{"root": process, "image", "class", "step"}],
//  "notifications": [{"id", "time", "root": process, "image", "class", "step", "reason",
//                     "members"}],
//  "orphans": {"enabled": bool, "min_trees": integer, "min_age_minutes": integer,
//              "proposals": [{"id", "time", "pattern", "image", "trees", "processes"}],
//              "rules": [{"id", "pattern", "image", "rule": "always"|"ignore", "time"}]},
//  "events": [event]}
// "metrics" is null before the first complete sample. "adopted": a job of a previous run,
// re-opened after a crash; its members are polled (no completion port). "trees" are the trees
// with a ladder step in effect. "notifications" are the owner's pending notifications (already
// limited to one per tree per [notifications] interval_minutes, and always one for a suspend),
// oldest first, each with an "id" that only grows: the tray shows a toast for each id above the
// last one it showed. One leaves the list when the owner chose for its tree (unless Windows
// refused a resume: the processes then stay suspended and the notification keeps Resume), or when
// the tree's root exited and none of its processes, in whatever tree they are now, carries a step
// any more; at most max_notifications are kept, the oldest dropped first, but never one whose
// tree still has a suspended process. "orphans": the orphaned polling loops (governor
// spec, "Orphaned Polling Loops"): each proposal is a pattern (the normalized command line of the
// trees' roots) with no rule and at least min_trees live orphaned trees, "End <trees> loops of
// <pattern>", with an "id" kept while it stays proposed; "rules" are the owner's stored decisions.
// "events" are the newest status_events events, newest first.
inline constexpr std::string_view governor_status_method = "governor.status";
inline constexpr std::size_t max_notifications = 32;
inline constexpr std::size_t status_events = 20;

// Parameters: since (integer, default 0), limit (integer 1-max_events_limit, default 100).
// Stored events whose time is at or after `since`, newest first: {"events": [event]}.
inline constexpr std::string_view governor_events_method = "governor.events";
inline constexpr int default_events_limit = 100;
inline constexpr int max_events_limit = 1000;

// TRAY ONLY. Parameters: pid (integer, required), created (integer, required), choice (token,
// required: "resume"|"kill"|"allow_once"|"always_allow"). The owner's answer to the notification
// of the tree whose root is that process (governor spec, "Rate-Limited Owner Notifications" and
// "Owner Exemptions Survive Continued Pressure"); recorded with actor "owner". always_allow adds
// the root's image to the owner's allow list (stored, protected at once). {"commands": integer}:
// how many governor commands the choice caused. An unknown choice fails with invalid_argument.
inline constexpr std::string_view governor_choose_method = "governor.choose";

// TRAY ONLY. Parameters: minutes (integer 0-max_pause_minutes, required). Pauses the governor:
// every step is rolled back and every tree it suspended is resumed, and no step is taken until
// the pause ends. 0 ends a pause now. The pause is stored: a restarted service keeps it until it
// ends. {"paused_until": integer|null}.
inline constexpr std::string_view governor_pause_method = "governor.pause";
inline constexpr int max_pause_minutes = 1440;

// TRAY ONLY. Parameters: override (token, required: "auto"|"on"|"off"). The game-mode override;
// a detected game stays protected in every state. {"active": bool, "build_cpu_percent"}.
inline constexpr std::string_view governor_game_mode_method = "governor.game_mode";

// TRAY ONLY. Parameters: state (token, required: the SHQueryUserNotificationState of the owner's
// session as the tray reads it: "not_present"|"busy"|"running_d3d_full_screen"|
// "presentation_mode"|"accepts_notifications"|"quiet_time"|"app"), foreground_pid (integer,
// required, 0 when no window has the foreground). Sent by the tray every second while it runs:
// the foreground process is protected and feeds fullscreen game detection; the first state after
// a pause of more than owner_absent_seconds means the owner logged on, and game detection
// re-reads the owner's launchers and GameConfigStore. A state older than user_state_seconds is
// ignored. {}.
inline constexpr std::string_view governor_user_state_method = "governor.user_state";

// TRAY ONLY. Parameters: id (integer, required), decision (token, required: "end_now"|"always"|
// "ignore"|"forget"). end_now, always and ignore answer the orphaned-loop proposal `id`: end_now
// ends its trees, always stores the pattern as "always" and ends its trees (later orphans of the
// pattern are ended as they appear), ignore stores the pattern as "ignore". forget removes the
// stored rule `id`. Recorded with actor "owner"; in observe mode an end is only recorded.
// {"trees": integer}: the trees ended. An unknown id fails with not_found, an unknown decision
// with invalid_argument.
inline constexpr std::string_view governor_orphans_method = "governor.orphans";
inline constexpr int user_state_seconds = 5;
inline constexpr int owner_absent_seconds = 60;

}  // namespace myclean::ipc
