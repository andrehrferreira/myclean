#include "core/tray_model/reports.hpp"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/reports.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/json_read.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;
using json_read::text;

// The members of a report or comparison not found: the reason and the months that exist.
ReportText not_found(const Json& reply, std::string title) {
  return ReportText{.found = false,
                    .title = std::move(title),
                    .markdown = {},
                    .message = text(reply, "message"),
                    .available = json_read::texts(reply, "available")};
}

}  // namespace

Result<std::vector<ReportRow>> parse_reports_list(const Json& reply) {
  return json_read::parsed(ipc::reports_list_method, [&] {
    std::vector<ReportRow> rows;
    for (const Json& report : reply.at("reports")) {
      rows.push_back(ReportRow{
          .month = report.at("month").get<std::string>(),
          .snapshot = text(report, "snapshot"),
          .generated_at = json_read::optional_time(report, "generated_at"),
          .against = text(report, "against"),
          .first_report = json_read::optional_bool(report, "first_report").value_or(false)});
    }
    return rows;
  });
}

Result<ReportText> parse_reports_get(const Json& reply) {
  return json_read::parsed(ipc::reports_get_method, [&] {
    const std::string title = "Report of " + reply.at("month").get<std::string>();
    if (!reply.at("found").get<bool>()) {
      return not_found(reply, title);
    }
    return ReportText{.found = true,
                      .title = title,
                      .markdown = reply.at("markdown").get<std::string>(),
                      .message = {},
                      .available = {}};
  });
}

Result<ReportText> parse_reports_compare(const Json& reply) {
  return json_read::parsed(ipc::reports_compare_method, [&] {
    if (!reply.at("found").get<bool>()) {
      return not_found(reply, "Changes between two snapshots");
    }
    return ReportText{.found = true,
                      .title = "Changes from " + reply.at("a").get<std::string>() + " to " +
                               reply.at("b").get<std::string>(),
                      .markdown = reply.at("markdown").get<std::string>(),
                      .message = {},
                      .available = {}};
  });
}

}  // namespace myclean::tray_model
