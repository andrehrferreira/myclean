#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"

// The stored monthly snapshots and reports (reports spec): tables `snapshot` and `report`, one
// row per month each, kept permanently (store retention never touches them).
namespace myclean::report {

// Component "report": the snapshot and report tables.
[[nodiscard]] const store::MigrationSet& report_migrations();

struct StoredReport {
  std::chrono::year_month month;
  WallTime generated_at;
  std::optional<std::chrono::year_month> against;  // the snapshot compared with; none: first
  std::filesystem::path markdown_file;
  std::filesystem::path html_file;
  std::string markdown;
  nlohmann::json diff;  // null for the first report
};

// Stores the snapshot of `report.month` and its report in one transaction. Fails with
// already_exists when that month has a snapshot.
[[nodiscard]] Result<void> save(store::Database& db, const nlohmann::json& snapshot,
                                const StoredReport& report);

// The snapshot of `month`; nullopt when none is stored.
[[nodiscard]] Result<std::optional<nlohmann::json>> load_snapshot(store::Database& db,
                                                                  std::chrono::year_month month);
// The newest snapshot of a month before `month`, with its month; nullopt when none is stored.
[[nodiscard]] Result<std::optional<std::pair<std::chrono::year_month, nlohmann::json>>>
newest_before(store::Database& db, std::chrono::year_month month);
// The report of `month`; nullopt when none is stored.
[[nodiscard]] Result<std::optional<StoredReport>> load_report(store::Database& db,
                                                              std::chrono::year_month month);

// The months with a snapshot (`snapshots`) or a report, newest first.
[[nodiscard]] Result<std::vector<std::chrono::year_month>> stored_months(store::Database& db,
                                                                         bool snapshots);

}  // namespace myclean::report
