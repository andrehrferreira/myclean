#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/snapshot.hpp"

// The tray's reading for the Audit and Settings screens: one page of the append-only action log
// (contract in core/ipc/audit.hpp), and the settings the service exposes, shown and never edited
// here.
namespace myclean::tray_model {

// One entry of audit.list.
struct AuditEntryRow {
  std::int64_t id = 0;
  WallTime time;
  std::optional<std::int64_t> plan;
  std::optional<std::int64_t> item;
  std::string actor;
  std::string event;
  std::string from;
  std::string to;
  std::string outcome;
  std::string reason;
  std::string detail;
  std::string metrics;
};
struct AuditPage {
  std::vector<AuditEntryRow> entries;  // newest first
  std::optional<std::int64_t> next;    // the "before" of the next, older page
};
[[nodiscard]] Result<AuditPage> parse_audit_list(const nlohmann::json& reply);
// audit.list of the page below `before` (the newest page when nullopt).
[[nodiscard]] nlohmann::json audit_list_params(std::optional<std::int64_t> before);
// What an entry records, in one line: "awaiting_approval -> approved: accepted", "item 3: deleted
// (freed=4096)", with its reason and detail.
[[nodiscard]] std::string audit_text(const AuditEntryRow& entry);

// The alerts the owner snoozed that are still snoozed at `now`.
[[nodiscard]] std::vector<const AlertRow*> snoozed_alerts(const std::vector<AlertRow>& alerts,
                                                          WallTime now);

}  // namespace myclean::tray_model
