#include "core/tray_model/audit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/audit.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/json_read.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

using Json = nlohmann::json;

Result<AuditPage> parse_audit_list(const Json& reply) {
  return json_read::parsed(ipc::audit_list_method, [&] {
    AuditPage page{.entries = {}, .next = json_read::optional_int(reply, "next")};
    for (const Json& entry : reply.at("entries")) {
      page.entries.push_back(
          AuditEntryRow{.id = entry.at("id").get<std::int64_t>(),
                        .time = from_unix_seconds(entry.at("time").get<std::int64_t>()),
                        .plan = json_read::optional_int(entry, "plan"),
                        .item = json_read::optional_int(entry, "item"),
                        .actor = entry.at("actor").get<std::string>(),
                        .event = entry.at("event").get<std::string>(),
                        .from = json_read::text(entry, "from"),
                        .to = json_read::text(entry, "to"),
                        .outcome = json_read::text(entry, "outcome"),
                        .reason = json_read::text(entry, "reason"),
                        .detail = json_read::text(entry, "detail"),
                        .metrics = json_read::text(entry, "metrics")});
    }
    return page;
  });
}

Json audit_list_params(std::optional<std::int64_t> before) {
  return before ? Json{{"before", *before}} : Json::object();
}

std::string audit_text(const AuditEntryRow& entry) {
  std::string text;
  if (!entry.from.empty() || !entry.to.empty()) {
    text = entry.from + " -> " + entry.to + ": ";
  } else if (entry.item) {
    text = "item " + std::to_string(*entry.item) + ": ";
  }
  text += entry.outcome;
  for (const std::string* part : {&entry.metrics, &entry.reason, &entry.detail}) {
    if (!part->empty()) {
      text += " (" + *part + ")";
    }
  }
  return text;
}

std::vector<const AlertRow*> snoozed_alerts(const std::vector<AlertRow>& alerts, WallTime now) {
  std::vector<const AlertRow*> snoozed;
  for (const AlertRow& alert : alerts) {
    if (alert.snoozed_until && *alert.snoozed_until > now) {
      snoozed.push_back(&alert);
    }
  }
  return snoozed;
}

}  // namespace myclean::tray_model
