#pragma once

#include <string_view>

// The audit-log method (NFR-SAFE-2; core/actions/audit_log.hpp). Times are Unix seconds (UTC).
// It only reads the append-only action_log.
namespace myclean::ipc {

// TRAY ONLY. Parameters: before (integer, optional): list only entries whose id is below it (the
// "next" of the previous page); limit (integer, optional, 1..audit_max_limit, default
// audit_default_limit). One page of action_log, newest first:
// {"entries": [{"id": int, "time": int, "plan": int|null (null when the request named no stored
//               plan), "item": int|null, "actor", "event", "from": string|null,
//               "to": string|null, "outcome", "reason", "detail", "metrics"}],
//  "next": int|null (the "before" of the next, older page; null after the oldest entry)}
// Events are those of actions::LogEntry: transition, approval, apply, restore, purge, verify and
// the governor's steps. Fails with invalid_argument for a limit out of range.
inline constexpr std::string_view audit_list_method = "audit.list";
inline constexpr int audit_default_limit = 50;
inline constexpr int audit_max_limit = 200;

}  // namespace myclean::ipc
