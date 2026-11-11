#pragma once

#include <string_view>

// The monthly report methods (reports spec; core/report, svc::ReportComponent). A month is the
// token "YYYY-MM" (2026-11), local time; each month has at most one snapshot and one report.
// Times are Unix seconds (UTC). Both methods only read the stored snapshots and reports.
namespace myclean::ipc {

// Diff (core/report/diff.hpp): {"from": month, "to": month,
//   "sets": {"startup_items"|"browser_extensions"|"services"|"listening_ports"|
//            "unsigned_binaries": {"added": [item], "removed": [item]} | {"not_compared": reason}},
//   "top_folders": {"grown": [{"path", "before", "after", "change"}], "new": [{"path", "after"}],
//                   "gone": [{"path", "before"}]} | {"not_compared": reason},
//   "disk_health": [{"key", "model", "status_before": string|null, "status_after": string|null,
//                    "changes": {counter: {"before", "after"}}}] | {"not_compared": reason},
//   "free_space": {"volume", "before", "after", "change"} | {"not_compared": reason},
//   "counts": {"governor_interventions"|"freeze_incidents"|"space_freed_bytes":
//              {"before": int|null, "after": int|null}}}
// Items are those of the snapshot's category; a listening port is {"key", "protocol", "port",
// "image", "pid"}. Sizes are bytes.

// No parameters. Every stored report, newest first:
// {"reports": [{"month", "snapshot": month (the id of its snapshot, which reports.compare
//               takes), "generated_at", "against": month|null, "first_report": bool,
//               "files": {"markdown", "html"}}]}
inline constexpr std::string_view reports_list_method = "reports.list";

// Parameters: month (token, required, "YYYY-MM"). The stored report of that month:
// {"found": true, "month", "generated_at", "against": month|null (the snapshot it was compared
//  with; null for the first report), "first_report": bool, "files": {"markdown", "html"},
//  "markdown": the report's text, "diff": Diff|null (null for the first report)}
// For a month without a report: {"found": false, "month", "message": "no report exists for
// <month>", "available": [months with a report, newest first]}. Fails with invalid_argument
// when `month` is not YYYY-MM.
inline constexpr std::string_view reports_get_method = "reports.get";

// Parameters: a (token, required), b (token, required): two snapshot months. The changes from
// snapshot `a` to snapshot `b`, in the categories of the monthly report:
// {"found": true, "a", "b", "diff": Diff, "markdown": the diff as the report shows it}
// When either snapshot does not exist: {"found": false, "missing": [month], "message": "no
// snapshot exists for <month>", "available": [months with a snapshot, newest first]}. Fails
// with invalid_argument when a month is not YYYY-MM.
inline constexpr std::string_view reports_compare_method = "reports.compare";

}  // namespace myclean::ipc
