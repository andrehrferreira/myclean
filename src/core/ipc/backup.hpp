#pragma once

#include <string_view>

// The backup method (backup spec; core/backup). Times are Unix seconds (UTC). It only reads:
// nothing here configures, starts or changes a backup, and the repository check never fetches,
// pushes or commits.
namespace myclean::ipc {

// Parameters: refresh (boolean, optional, default false). The latest backup check, which the
// backup component runs at its start, once a day and on request:
// {"state": "checking"|"ready"|"failed", "checked_at": int|null, "error": string|null,
//  "sources": [{"source": "file_history"|"system_image"|"restore_points"|"shadow_copies"|
//               "onedrive"|"third_party"|"backup_drives",
//               "name", "status": "found"|"not_found"|"could_not_check",
//               "location": string|null, "newest": int|null, "newest_age_days": int|null,
//               "copies": int, "counts_toward_age": bool, "detail": string|null,
//               "products"?: [string] (third_party), "folders"?: [string] (onedrive),
//               "artifacts"?: [{"kind": "macrium_image"|"veeam_backup"|"acronis_backup"|
//                               "system_image"|"file_history"|"manual_copy"|"dump",
//                               "path", "newest"}] (newest first, at most 50)}],
//  "last_backup": null | {"source", "name", "location": string|null, "newest", "age_days"},
//  "backup_exists": bool, "summary": string,
//  "recommendation": null | {"text", "drives": [string], "left_out": [{"drive", "reason"}]},
//  "repositories": null | {"checked": int, "unprotected": [{"path", "no_remote": bool,
//                  "unpushed": int, "uncommitted": bool, "text"}], "problems": [string],
//                  "truncated": bool}}
// Before the first check ends the report members are absent. "status" could_not_check means the
// source's read failed ("detail" holds the error), not that nothing was found. Restore points and
// shadow copies are listed but never count toward the last-backup age. "recommendation" is set
// only when no backup exists and names only drives that exist and are healthy; MyClean never
// sets a backup up itself. With refresh, or when the last check is older than
// backup_result_hours, a new check starts in the background ("state": "checking").
inline constexpr std::string_view backups_check_method = "backups.check";
inline constexpr int backup_result_hours = 24;

}  // namespace myclean::ipc
