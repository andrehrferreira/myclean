#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// The cleaning, plan, user-task, quarantine and space methods of the service (cleaning,
// approvals and space specs). Every answer is a JSON object; times are Unix seconds (UTC),
// sizes bytes, paths UTF-8 text. Methods marked TRAY ONLY answer every other caller (the CLI,
// `myclean mcp`, any other pipe client) with error -32000, errc "refused", and run nothing:
// the service accepts them only from the configured myclean-tray.exe running as the owner in an
// interactive session (checked per request from the pipe client's process id).
namespace myclean::ipc {

// ---- Shapes shared by several answers ---------------------------------------------------------
//
// PlanSummary:
//   {"id": int, "title": string,
//    "kind": "cleanup"|"handler"|"native"|"relocate"|"system_setting"|"quarantine_restore"|
//            "recycle_bin"|"archive"|"uninstall"|"compact"|"pause",
//    "tier": "auto"|"ask"|"report", "scope": "user"|"service",
//    "state": "proposed"|"auto"|"awaiting_approval"|"approved"|"applying"|"applied"|"verified"|
//             "rolled_back"|"failed"|"rejected",
//    "proposer": "scheduler"|"governor"|"owner"|"mcp:<client>", "created_at": int,
//    "items": int (count), "bytes": int (total size of the items), "needs_restart": bool,
//    "verification": null|"pending"|"better"|"unchanged"|"worse"|"error"}
//   kind: cleanup = files deleted, quarantined or recycled; handler = Windows disk-cleanup
//   handlers or DISM; native = a package manager's prune command; relocate = a cache or TEMP
//   moved through its setting; system_setting = hibernation, page file or shadow storage;
//   quarantine_restore = quarantined items moved back; recycle_bin = the Recycle Bin items the
//   owner deleted long ago, deleted permanently; archive = a cold folder moved to the HDD archive;
//   uninstall = an installed program's own uninstaller started (the last two: the owner's
//   one-click decisions, plans.owner_action); compact = WSL and Docker virtual disks compacted
//   (plans.propose_compact; not reversible); pause = a freeze suspect paused for a while
//   (plans.propose_pause).
//   scope: "service" plans run in the service; "user" plans run in the tray with the owner's
//   token (user_tasks.*).
//   verification: the newest re-measurement of the metric the plan targets (optimizer spec,
//   "Before-and-After Verification"), null before the plan was applied. "pending": the metric is
//   measured again later (the next logon after a startup change, a day of indexer use after an
//   index exclusion); the plan stays applied and is not counted as verified. An optimization
//   (kind system_setting or compact) whose metric got worse is undone at once (rolled_back) when
//   it is reversible, and an alert tells the owner (data/alerts.toml, rules plan.*).
//
// PlanItem:
//   {"id": int, "path": string, "bytes": int, "rule": string|null, "reason": string,
//    "approved": true|false|null (null: not decided yet)}
//   path is a file system path, or "handler:<name>" / "dism:component-store" /
//   "relocate:<cache>" / "relocate:<pack>/<case>[:<instance>]" /
//   "recycle-bin:deleted-<days>-days-ago" / "uninstall:<program>" for items that are not files.

// ---- cleaning.* -------------------------------------------------------------------------------

// No parameters. Starts a cleaning scan in the background (rules, developer artifacts, Windows
// handler sizes) unless one runs or cleaning is unavailable. The scan stores its candidates,
// creates an `awaiting_approval` plan per ask-tier group and records report-tier candidates as
// findings; its auto-tier items are deleted at once, without approval. The scheduled auto run
// (hourly with catch-up, skipped while the system volume is critically busy) scans and deletes
// them too.
//   {"started": bool}
inline constexpr std::string_view cleaning_scan_method = "cleaning.scan";

// No parameters.
//   {"state": "idle"|"scanning", "progress": number 0..1, "last_completed": int|null,
//    "candidates": int, "bytes": int (of those candidates), "next_auto_run": int|null,
//    "purging": [int] (quarantine runs queued by quarantine.purge, the first being deleted),
//    "applying": null | {"kind": "plan"|"purge", "plan": int|null, "title": string,
//                        "started_at": int, "items_done": int, "items_total": int,
//                        "bytes_done": int, "bytes_total": int, "files_done": int,
//                        "current": string|null},
//    "unavailable": string|null (why scans or executions cannot run now)}
// applying: what the service applies now, null when nothing. kind "plan": an approved plan (its
// id in "plan", its title); "purge": a quarantine purge ("plan" the run deleted now, null for
// the scheduled purge of expired runs and runs on a volume short of space). Items and bytes
// count the plan's items (a purge: one run folder per volume) and their stored sizes, done
// when acted on, skipped or refused; files_done counts files removed, moved or copied;
// current is the item handled now. A service older than this field omits it.
inline constexpr std::string_view cleaning_status_method = "cleaning.status";

// ---- plans.* ----------------------------------------------------------------------------------

// Parameters: state (token, optional): one state's text form. The newest max_listed_plans
// plans, newest first: {"plans": [PlanSummary]}
inline constexpr std::string_view plans_list_method = "plans.list";
inline constexpr std::size_t max_listed_plans = 500;

// Parameters: id (integer, required). The PlanSummary members, with "items" being the item list:
//   {...PlanSummary, "items": [PlanItem] (the first max_plan_items), "items_total": int,
//    "commands": [string] (exactly what runs), "undo": string (how it is undone, or why it
//    cannot be), "warnings": [string], "params": object (the action parameters),
//    "freed": int|null (bytes freed, once applied), "detail": string|null (why it failed,
//    was rejected or rolled back), "verification": null | {"result": "pending"|"better"|
//    "unchanged"|"worse"|"error", "metric": string|null, "before": number|null, "after":
//    number|null (null while pending), "time": int, "detail": string|null (why it could not be
//    measured)}}
// Fails with not_found for an unknown id.
inline constexpr std::string_view plans_get_method = "plans.get";
inline constexpr std::size_t max_plan_items = 2000;

// TRAY ONLY. Parameters: id (integer, required), selection (record, required):
//   {"all": bool, "items": [int], "except": [int]}  (each list optional, at most 64 ids)
// all=true approves every item but those in "except"; all=false approves only "items".
// Approving none rejects the plan. A service-scope plan then runs in the background; a
// user-scope plan waits for user_tasks.next. Answers {"state": "approved"|"rejected"}; fails
// with invalid_state when the plan does not await approval, invalid_argument for an unknown
// item id.
inline constexpr std::string_view plans_approve_method = "plans.approve";

// TRAY ONLY. Parameters: id (integer, required). Rejects every item: {"state": "rejected"}.
inline constexpr std::string_view plans_reject_method = "plans.reject";

// TRAY ONLY. Parameters: id (integer, required). Undoes an applied or verified plan: a
// service-scope plan now ({"state": "rolled_back"}), a user-scope one through the tray
// (user_tasks.next hands out the undo; {"state": <unchanged>, "queued": true}). Fails with
// not_reversible when the change cannot be undone (deleted files, handler and DISM cleanups).
inline constexpr std::string_view plans_undo_method = "plans.undo";

// Parameters: candidates (record, required): {"ids": [int]} (1..64 candidate ids from
// space.* or a cleaning scan; never paths), client (token, optional; default "unknown").
// Creates one quarantine plan over those items, in state awaiting_approval, proposed by
// "mcp:<client>" (by "owner" when the caller is the tray). Nothing is touched until the owner
// approves it in the tray. {"plan": int, "state": "awaiting_approval"}. Fails with not_found
// for an unknown or stale candidate id and invalid_argument for one that is not a file item.
inline constexpr std::string_view plans_propose_cleanup_method = "plans.propose_cleanup";

// Parameters: kind (token, required): "hibernation" | "page_file" | "shadow_storage" |
// "relocate"; client (token, optional) as above; and per kind (each optional, the recommended
// value when absent):
//   hibernation      mode (token): "off" | "reduced" | "full"
//   page_file        target (token): drive letter ("D") for the system-managed page file
//   shadow_storage   volume (token): drive letter (default the system drive);
//                    maximum_gb (number > 0)
//   relocate         cache (token): "temp"|"gradle"|"cargo"|"npm"|"nuget"|"pip"|
//                    "go_modules"|"go_build"|"unreal_ddc"; or pack and recipe (tokens): a
//                    relocation recipe "<pack>/<case>" of the packs in effect, with instance
//                    (token) for a recipe whose setting names instances (a WSL distribution);
//                    target (token): drive letter; content (token): "move" | "regenerate"
// Creates an awaiting_approval plan (system settings never run without approval):
// {"plan": int, "state": "awaiting_approval"}. Fails with invalid_state when nothing would
// change (already at the requested value, or no healthy target with room).
inline constexpr std::string_view plans_propose_optimization_method = "plans.propose_optimization";

// No parameters. Every startup item of the owner and of the machine (startup::inventory), read
// now by the service: {"items": [{"id": int, "key": string, "source", "scope", "name",
// "display_name", "command", "enabled": bool|null, "signer"?, "start"?, "delayed"?, "impact"}]}.
// "id" is the item's number in this answer, the only way plans.propose_startup names it;
// "enabled" is null for RunOnce items, which cannot be disabled. "impact" is null when unknown,
// else {"source", "cpu_seconds", "io_bytes", "logons", "delay_seconds"}: with source "myclean",
// the median use of the item's program in the first 2 minutes after the latest logons MyClean
// measured (cpu_seconds, io_bytes, logons; delay_seconds 0); with source "diagnostics", for a
// program none of those logons ran, the largest boot delay Windows attributed to it in the last
// 30 days (Diagnostics-Performance event 101, DegradationTime; the others 0).
inline constexpr std::string_view startup_list_method = "startup.list";

// No parameters. The duration of each boot and of the logon it led to, as Windows measured them
// (Diagnostics-Performance event 100, recorded in table boot_time; durations leave out the time
// the logon screen waited): {"source": "diagnostics", "log_error": string|null (the log could
// not be read now; recorded boots are still shown), "latest": null | {"start": int (unix
// seconds), "boot_seconds", "main_path_seconds", "post_boot_seconds", "logon_seconds" (profile
// load + Explorer start + post-boot), "boot_difference_seconds", "logon_difference_seconds"
// (from the median; null without one), "logon": int|null (the logon MyClean measured for that
// boot), "contributors": [{"image", "cpu_seconds", "io_bytes", "item": null | {"name", "source",
// "scope"}}] (what each program used in the first 2 minutes of that logon, most disk bytes
// first, then most CPU; at most 10; "item" is the startup item that runs it)}, "median": null |
// {"boot_seconds", "logon_seconds", "boots"} (over the boots of the last 30 days), "changes":
// [{"plan": int, "applied": int, "items": [{"name", "source", "scope"}], "before", "after"}]}
// (each startup change in effect that disabled items, newest first; "before" and "after" are
// null or {"logon_seconds", "boots"}: the median logon of the latest 5 boots before it and of
// the first 5 boots after it).
inline constexpr std::string_view startup_boot_times_method = "startup.boot_times";

// Parameters: candidates (record, required): {"ids": [int]}, "id" values of the latest
// startup.list answer; enable (boolean, optional, default false); client (token, optional).
// Creates an awaiting_approval plan that disables (or enables) those items the way Windows does:
// {"plan": int, "state": "awaiting_approval"}. Fails with not_found for an id that answer did not
// list, and with invalid_argument for an item that cannot change that way.
inline constexpr std::string_view plans_propose_startup_method = "plans.propose_startup";

// Parameters: candidates (record, required): {"ids": [int]}, "id" values of
// space.indexed_folders; client (token, optional). Creates an awaiting_approval plan that keeps
// those folders out of the Windows Search index with one exclusion rule each (the folders and
// their files are not touched; undo restores the index scope exactly):
// {"plan": int, "state": "awaiting_approval"}. Fails with not_found for an unknown or stale
// id, invalid_argument for a candidate that is no indexed folder, and invalid_state for a
// folder the index no longer holds or when Windows Search cannot be reached.
inline constexpr std::string_view plans_propose_index_exclusion_method =
    "plans.propose_index_exclusion";

// Parameters: candidates (record, required): {"ids": [int]}, "id" values of the latest
// space.vhdx answer; client (token, optional). Creates an awaiting_approval plan (kind compact,
// action CompactVhdx, run by the service) that, once the owner approves it in the tray, runs
// `wsl --shutdown` as the owner (every running WSL distribution and Docker container stops: the
// plan's warnings say so) and then compacts each disk, recording its size before and after:
// {"plan": int, "state": "awaiting_approval"}. Fails with not_found for an unknown candidate id
// and invalid_argument for a candidate that is not a disk of space.vhdx.
inline constexpr std::string_view plans_propose_compact_method = "plans.propose_compact";

// Parameters: suspect (integer, required): an "id" of input.pauses' suspects; hours (integer,
// optional; 1..168, default 24); client (token, optional). Creates an awaiting_approval plan
// (kind pause, action PauseSuspect, run by the service once the owner approves it in the tray)
// that pauses the suspect for that long, mode guided for a suspect that owns a keyboard hook
// (the owner quits it), suspend otherwise (see input.pauses): {"plan": int, "state":
// "awaiting_approval"}. An approved pause waits while another one is active. Fails with
// invalid_state while the baseline is still being collected, not_found for an unknown suspect,
// invalid_argument for hours out of range, already_exists while a pause plan of the suspect is
// open.
inline constexpr std::string_view plans_propose_pause_method = "plans.propose_pause";

// TRAY ONLY. Parameters: candidates (record, required): {"ids": [int]}. The owner deletes
// report-view items (a duplicate, an old download) to the Recycle Bin: an approved user-scope
// cleanup plan, run by the tray through IFileOperation with FOFX_RECYCLEONDELETE.
// {"plan": int, "state": "approved"}
inline constexpr std::string_view plans_recycle_method = "plans.recycle";

// TRAY ONLY. Parameters: candidate (integer, required): an id of space.owner_decisions. The
// owner's one click on an owner decision, which is the approval of exactly that plan: an approved
// user-scope plan, run by the tray with the owner's token (user_tasks.next):
//   a cold folder   kind "archive": copied to its archive folder "<HDD>\Archive\<letter>\<path>"
//                   on the healthy HDD with the most room that keeps 10 % free (judged now), the
//                   copy checked to hold the same files and bytes, then the folder removed
//                   through SafeRemove; undo copies it back and removes the archive copy;
//   a program       kind "uninstall": its Uninstall entry's UninstallString started on the
//                   owner's desktop (the program's own interactive uninstaller); not reversible;
//                   verified when the entry is gone, else "launched; finish in the uninstaller".
// {"plan": int, "state": "approved"}. Fails with not_found for an unknown candidate or one gone
// since the list was made, invalid_argument for a candidate of another list, refused for a
// system or deny-list place, invalid_state when no healthy HDD has room.
inline constexpr std::string_view plans_owner_action_method = "plans.owner_action";

// ---- user_tasks.* (the tray runs user-scope plans with the owner's token) ---------------------

// TRAY ONLY. No parameters. The oldest user-scope plan to run now, or null:
//   {"task": null | {"plan": int, "title": string,
//    "kind": "native"|"handler"|"relocate"|"cleanup"|"recycle_bin"|"archive"|"uninstall",
//    "operation": "apply"|"undo",
//    "tier": "auto"|"ask"|"report",
//    "items": [{"id": int, "path": string, "bytes": int, "approved": bool|null,
//               "volume_serial": int, "file_id": hex string (both 0/"" for non-files)}],
//    "action": {"command"}                                   (native)
//            | {"volume", "handlers": [name]}                (handler; user-profile handlers)
//            | {"cache", "target", "content": "move"|"regenerate"}   (relocate)
//            | {"recipe": {the recipe's fields}, "instance", "target", "content"}
//                                                             (relocate by a recipe)
//            | {"recycle": true}                              (cleanup: to the Recycle Bin)
//            | {"deleted_days": int}  (recycle_bin: the items of every fixed volume's bin deleted
//                                      at least that many days ago, deleted permanently)
//            | {"source", "target"}                       (archive: the folder, its archive folder)
//            | {"key", "name"}       (uninstall: the program's Uninstall key and its DisplayName),
//    "undo": {"not_reversible": string, "items": [{"item", "data"}]} (undo only: as reported)}}
// An apply task lists the items not rejected; the tray acts on no other. A task handed out is
// not handed out again for task_claim_minutes unless reported.
inline constexpr std::string_view user_tasks_next_method = "user_tasks.next";
inline constexpr int task_claim_minutes = 60;

// TRAY ONLY. Parameters: id (integer, required: the plan), result (record, required):
//   {"state": "applied"|"verified"|"rolled_back"|"failed",
//    "error": string|null (why it failed, was rolled back, or could not be verified),
//    "not_reversible": string (optional: why the change cannot be undone; empty if it can),
//    "undo": [{"item": int, "data": string}] (optional: undo data per plan item id;
//             consecutive entries of one item are concatenated, so data longer than a record
//             string is sent in pieces),
//    "verification": {"metric": string, "before": number, "after": number,
//                     "effect": "better"|"unchanged"|"worse"}|null (optional),
//    "skipped": [{"target": string, "code": string, "message": string}] (optional)}
// The service records it by the plan's state: an approved plan reports its apply (applying,
// applied with the undo data, the verification, then verified, or rolled_back when the tray
// undid a worse result, or failed); an applied or verified plan reports its undo
// (rolled_back, or the failure logged). Actor: owner. {"state": <new state>}. Fails with
// invalid_state when the plan has no task to report.
inline constexpr std::string_view user_tasks_report_method = "user_tasks.report";

// The schema checks of the record parameters above: the reason a record does not fit, or "".
[[nodiscard]] std::string check_selection(const nlohmann::json& record);
[[nodiscard]] std::string check_candidates(const nlohmann::json& record);
[[nodiscard]] std::string check_task_result(const nlohmann::json& record);

// ---- quarantine.* -----------------------------------------------------------------------------

// No parameters. Every run with items still held:
//   {"runs": [{"plan": int, "moved_at": int, "purge_after": int, "bytes": int,
//              "items": [{"item": int, "original": string, "location": string, "bytes": int}]}]}
// Held items are purged (permanently deleted) after purge_after; restore moves them back.
inline constexpr std::string_view quarantine_list_method = "quarantine.list";

// TRAY ONLY. Parameters: plan (integer, required: the run), item (integer, optional: one plan
// item; every held item of the run when absent). {"restored": int}. Fails with io_error when
// an item could not be moved back (it stays in quarantine).
inline constexpr std::string_view quarantine_restore_method = "quarantine.restore";

// TRAY ONLY. Parameters: plan (integer, required: the run). Queues the permanent deletion of
// every held item of the run now, before its purge date: the owner frees the space (a
// quarantine on a nearly full volume frees nothing until then). The cleaning worker purges it
// before the next approved plan; cleaning.status lists it under "purging" until done, and
// quarantine.list then no longer has it. {"queued": true}. Fails with not_found when the run
// holds no item.
inline constexpr std::string_view quarantine_purge_method = "quarantine.purge";

// ---- space.* (read-only analyses, run by the service in the background) -----------------------
//
// Each answer carries the newest result and the state of its analysis:
//   {"state": "ready"|"scanning"|"failed", "scanned_at": int|null, "error": string|null, ...}
// A call starts a new analysis when there is no result, the result is older than
// space_result_minutes or refresh is true; while it runs, "state" is "scanning" and the
// previous result (if any) is returned. Call again until "ready". Every file entry carries an
// "id": a candidate id for plans.propose_cleanup and plans.recycle.

// Parameters: path (token, required): a drive letter ("C") or "downloads" or "profile" (the
// owner's); n (integer 1..100, optional, default 20); refresh (boolean, optional).
//   {..., "path": string, "files": [{"id", "path", "size_on_disk", "size", "modified",
//    "protected"?}], "folders": [{"path", "size_on_disk", "size", "files"}]}
// "protected" names the deny-list entry that refuses the file (core/safety/deny_list.hpp, such
// as "page_file" or "system_volume_information"); a plan of it would be refused, so it is shown
// but not proposed.
inline constexpr std::string_view space_top_method = "space.top";

// Parameters: refresh (boolean, optional). Files of the owner's Downloads folder created and
// modified more than 90 days ago:
//   {..., "folder": string|null, "files": [{"id", "path", "size", "size_on_disk", "created",
//    "modified", "age_days", "origin": {"zone_id", "host_url", "referrer_url"}|null}]}
inline constexpr std::string_view space_old_downloads_method = "space.old_downloads";

// Parameters: refresh (boolean, optional), mode (token, optional): "exact" (default) or "photos".
// Duplicates in the owner's Desktop, Documents, Downloads, Pictures, Videos and Music.
// mode exact, files of equal content:
//   {..., "mode": "exact", "roots": [string], "groups": [{"size", "hash", "reclaimable",
//    "files": [{"id", "path", "size_on_disk", "modified"}]}], "reclaimable": int,
//    "errors": [{"path", "error"}], "groups_total": int, "truncated": bool}
// mode photos, near-duplicate photos (images of at least 16 KiB decoded with WIC, grouped when
// their 64-bit dHash values differ in at most 6 bits, or are linked through such pairs):
//   {..., "mode": "photos", "roots": [string], "groups": [{"distance" (the most bits two of
//    its photos differ in), "reclaimable", "files": [{"id", "path", "size_on_disk", "modified",
//    "width", "height", "hash" (hex), "keep": bool}]}], "reclaimable": int, "analysed": int,
//    "not_analysed": [{"path", "error"}] (files no codec decoded), "groups_total": int,
//    "truncated": bool}
//   The first file of a group is the one to keep (the most pixels, then the largest file).
// groups: the most reclaimable first, as many as fit 3/4 of one pipe message (truncated: some
// were left out; every file of every group is still a candidate). Each mode is its own analysis
// with its own result. Fails with invalid_argument for another mode.
inline constexpr std::string_view space_duplicates_method = "space.duplicates";

// Parameters: refresh (boolean, optional). Installers in the owner's Downloads that are
// installed already:
//   {..., "folder": string|null, "used": [{"id", "path", "kind": "msi"|"exe", "size",
//    "size_on_disk", "installed_as", "product_code"|"publisher"}], "errors": [...]}
inline constexpr std::string_view space_used_installers_method = "space.used_installers";

// Parameters: refresh (boolean, optional). What only the owner can decide (owner-decisions spec),
// each with its one-click action (plans.owner_action):
//   {..., "profile": string|null, "cold": [{"id", "path", "bytes", "target": string|null,
//    "reason", "protected"?}], "uninstall": [{"id", "program", "publisher", "version",
//    "location", "bytes", "area", "reason"}], "notes": [string]}
// cold: folders of the owner's profile on an SSD of at least 1 GiB that nothing has written for
// six months (scan-all's cold rule), outermost first; "target" is the archive folder they would
// go to, null when no healthy HDD keeps 10 % free after taking it; "protected" names the
// deny-list entry that refuses the folder (no action is possible). uninstall: the installed
// programs (Uninstall entries of HKLM and the owner's HKCU) whose InstallLocation lies in an
// area the knowledge packs mark for uninstall review, largest first (bytes: EstimatedSize, else
// the install folder's size on disk), at most max_owner_programs. notes: what could not be
// judged, and why.
inline constexpr std::string_view space_owner_decisions_method = "space.owner_decisions";
inline constexpr std::size_t max_owner_programs = 100;

// Parameters: refresh (boolean, optional). The Dev Drive advisor (placement spec): advice only,
// it never creates, formats or resizes a volume and moves nothing.
//   {..., "supported": bool (Windows 11 build 22621.2338 or later), "build": "<build>.<UBR>",
//    "dev_drives": [{"volume", "file_system", "trusted", "note"}] (the Dev Drives present),
//    "explanation": string (what a Dev Drive is), "repositories": [{"path", "bytes", "changed"}]
//    (git repositories below the workspaces.toml roots changed in the last 30 days, not on a
//    Dev Drive), "caches": [{"area", "label", "path", "bytes", "relocation", "note"}] (the
//    package caches the knowledge packs name, not on a Dev Drive; "relocation" is the cache
//    plans.propose_optimization kind relocate moves, "" when MyClean does not move it),
//    "total_bytes", "recommended_bytes" (what would move plus half again, at least 50 GB),
//    "host": null | {"volume", "free", "drive_class"} (where a new Dev Drive VHDX would go: the
//    healthy NVMe or SATA SSD with the most room that keeps 10 % free), "steps": [string] (what
//    the owner does, in order), "notes": [string] (what could not be judged), "sources": [string]}
inline constexpr std::string_view space_dev_drive_method = "space.dev_drive";

// Parameters: refresh (boolean, optional). Every WSL and Docker virtual disk (.vhdx) where the
// knowledge packs say they are (the registered folder of each WSL distribution, the vm-disks
// areas), largest first:
//   {..., "disks": [{"id", "path", "area", "label", "distribution": string|null, "size",
//    "size_on_disk", "in_use"}], "total": int, "notes": [string]}
// "id" is what plans.propose_compact takes; "in_use": a process holds it open (its
// distribution or Docker Desktop runs).
inline constexpr std::string_view space_vhdx_method = "space.vhdx";

// Parameters: refresh (boolean, optional). The build-output and cache folders Windows Search
// indexes (core/indexer): the existing folders of the knowledge packs' attribution areas of
// categories build-artifacts, dev-caches and app-caches, and the targets of their project clean
// cases in the projects below the workspace roots, that the crawl scope includes (a folder
// inside another one listed is left out):
//   {..., "folders": [{"id", "path", "category", "label"}]}
// "id" is what plans.propose_index_exclusion takes. Fails ("failed") when Windows Search is
// not running.
inline constexpr std::string_view space_indexed_folders_method = "space.indexed_folders";

inline constexpr int space_result_minutes = 60;
inline constexpr int max_space_top = 100;

}  // namespace myclean::ipc
