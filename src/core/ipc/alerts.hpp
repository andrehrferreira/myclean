#pragma once

#include <string_view>

// The alert methods of the service (alerts spec). Times are Unix seconds (UTC). Entities are in
// token form (entity_token): "C" for the volume "C:", the drive key of disks.health for a drive.
namespace myclean::ipc {

// Parameters: since (integer, default 0: every stored alert). Stored alerts whose last
// occurrence is at or after `since`, newest first:
// {"alerts": [{"rule", "entity", "title", "severity": "info"|"warning"|"critical",
//              "first_at", "last_at", "count", "active": bool, "snoozed_until": integer|null,
//              "toast_at": integer|null, "detail": text, "value": number,
//              "limit": number|null, "previous": number|null, "unit": text}]}
// "toast_at" is when the engine last decided this alert deserves a toast (after rate limiting
// and snooze); the tray shows a toast for each toast_at newer than the last one it saw. It is
// null when no toast was decided since the service started.
inline constexpr std::string_view alerts_list_method = "alerts.list";

// No parameters. {"count": active alerts that are not snoozed, "worst": "none"|"info"|
// "warning"|"critical"}.
inline constexpr std::string_view alerts_badge_method = "alerts.badge";

// Parameters: rule (token, required), entity (token, required), minutes (integer 1-43200,
// required). No toast and no badge for that alert for `minutes`; its occurrences are still
// recorded. {"snoozed_until"}. Fails with not_found when no stored alert has that rule and
// entity.
inline constexpr std::string_view alerts_snooze_method = "alerts.snooze";
inline constexpr int max_snooze_minutes = 43'200;  // 30 days

// Parameters: rule (token, required), entity (token, required). The owner has seen the alert:
// it leaves the active set until it fires again. {} on success; not_found as for snooze.
inline constexpr std::string_view alerts_dismiss_method = "alerts.dismiss";

inline constexpr std::string_view badge_none = "none";

}  // namespace myclean::ipc
