#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The tray's reading of the monthly report methods for the Reports screen (contracts in
// core/ipc/reports.hpp): the stored reports, one report's text, and the changes between two
// snapshots. They only read.
namespace myclean::tray_model {

// One report of reports.list.
struct ReportRow {
  std::string month;     // "2026-11"
  std::string snapshot;  // the month of its snapshot, which reports.compare takes
  std::optional<WallTime> generated_at;
  std::string against;  // the snapshot it was compared with; empty for the first report
  bool first_report = false;
};
[[nodiscard]] Result<std::vector<ReportRow>> parse_reports_list(const nlohmann::json& reply);

// A report (reports.get) or a comparison (reports.compare) as the screen shows it.
struct ReportText {
  bool found = false;
  std::string title;                   // "Report of 2026-11", "Changes from 2026-10 to 2026-11"
  std::string markdown;                // the text, when found
  std::string message;                 // why it was not found
  std::vector<std::string> available;  // the months that exist, when not found
};
[[nodiscard]] Result<ReportText> parse_reports_get(const nlohmann::json& reply);
[[nodiscard]] Result<ReportText> parse_reports_compare(const nlohmann::json& reply);

}  // namespace myclean::tray_model
