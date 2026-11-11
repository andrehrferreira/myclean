#pragma once

#include <string_view>

// The installed-app methods (apps spec; core/apps). Times are Unix seconds (UTC), sizes bytes.
// apps.list only reads; plans.propose_uninstall only creates a plan that waits for the owner's
// approval in the tray, and names the app only by its id in the latest apps.list answer.
namespace myclean::ipc {

// Parameters: unused_days (integer, optional, 1..3650): only the apps not used in that many days
// and those with no usage recorded. Every installed app of the Uninstall keys (HKLM 64-bit, HKLM
// 32-bit, the owner's HKCU; the entries Settings > Apps shows) and the owner's Appx packages,
// read now:
// {"apps": [{"id": int, "kind": "win32"|"appx", "name", "publisher": string|null,
//            "version": string|null, "install_date": "YYYY-MM-DD"|null, "size": int|null,
//            "description": string ("unknown" when the knowledge base does not know it),
//            "category": string|null,
//            "last_used": {"time": int|null, "days_ago": int|null,
//                          "source": "UserAssist"|"Prefetch"|"BAM"|"MyClean process history"|null,
//                          "confidence": "high"|"medium"|"low", "text": string},
//            "verdict": "removable"|"keep"|"unknown", "reason": string, "uninstaller": bool}],
//  "count": int (apps installed, before the filter), "unused_days": int|null,
//  "history": {"since": int|null, "days": int} (how far MyClean's process history reaches),
//  "manufacturer": string|null, "problems": [string] (sources that could not be read)}
// "id" is the app's number in this answer, the only way plans.propose_uninstall names it. A
// "time" of null is "no usage recorded" (never a guessed date); "text" then states the span of
// MyClean's history. "uninstaller" is false for a Win32 app with no UninstallString or
// QuietUninstallString.
inline constexpr std::string_view apps_list_method = "apps.list";
inline constexpr int apps_max_unused_days = 3650;

// Parameters: app (integer, required): an "id" of the latest apps.list answer; client (token,
// optional). Creates an awaiting_approval plan (kind "uninstall", run by the tray as the owner)
// that launches the app's own uninstaller, the UninstallString MyClean read from the registry
// (or removes the Appx package through PackageManager.RemovePackageAsync): {"plan": int,
// "state": "awaiting_approval", "verdict", "reason"}. A plan for an app whose verdict is keep
// carries the keep reason as a warning the tray shows. Nothing runs until the owner approves it in
// the tray, and it cannot be undone except by installing the app again. Fails with not_found for
// an id that answer did not list or an app uninstalled since, and with invalid_argument and the
// reason "no uninstaller registered" for a Win32 app without one.
inline constexpr std::string_view plans_propose_uninstall_method = "plans.propose_uninstall";

}  // namespace myclean::ipc
