#include "core/report/repository.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/report/month.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"

namespace myclean::report {
namespace {

using Json = nlohmann::json;

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

std::filesystem::path from_utf8(const std::string& text) {
  return std::u8string(text.begin(), text.end());
}

// Parses a stored JSON column; invalid_data when it is not JSON.
Result<Json> parse_column(const std::string& text, std::string_view what) {
  Json value = Json::parse(text, nullptr, false);
  if (value.is_discarded()) {
    return fail(Errc::invalid_data, "the stored " + std::string(what) + " is not JSON");
  }
  return value;
}

Result<std::optional<std::chrono::year_month>> month_column(const store::Statement& row,
                                                            int column) {
  if (row.is_null(column)) {
    return std::optional<std::chrono::year_month>{};
  }
  const std::string text = row.column_text(column);
  const std::optional<std::chrono::year_month> month = parse_month(text);
  if (!month) {
    return fail(Errc::invalid_data, "the stored month '" + text + "' is not YYYY-MM");
  }
  return month;
}

}  // namespace

const store::MigrationSet& report_migrations() {
  static const store::MigrationSet set{
      .component = "report",
      .migrations = {
          {.version = 1, .name = "snapshot_and_report", .sql = R"sql(
CREATE TABLE snapshot(
  month TEXT PRIMARY KEY,
  taken_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE TABLE report(
  month TEXT PRIMARY KEY,
  generated_at INTEGER NOT NULL,
  against TEXT,
  markdown_path TEXT NOT NULL,
  html_path TEXT NOT NULL,
  markdown TEXT NOT NULL,
  diff TEXT NOT NULL);
)sql"},
      }};
  return set;
}

Result<void> save(store::Database& db, const Json& snapshot, const StoredReport& report) {
  const std::string month = month_text(report.month);
  return db.transaction([&]() -> Result<void> {
    Result<std::optional<Json>> existing = load_snapshot(db, report.month);
    if (!existing) {
      return tl::unexpected(existing.error());
    }
    if (*existing) {
      return fail(Errc::already_exists, "a snapshot of " + month + " is stored already");
    }
    Result<store::Statement> insert =
        db.prepare("INSERT INTO snapshot(month, taken_at, data) VALUES(?1, ?2, ?3)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    const std::string data = snapshot.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (Result<void> ran = insert->bind(1, month)
                               .bind(2, store::to_unix_seconds(report.generated_at))
                               .bind(3, data)
                               .run();
        !ran) {
      return ran;
    }
    Result<store::Statement> row = db.prepare(
        "INSERT INTO report(month, generated_at, against, markdown_path, html_path, markdown, "
        "diff) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!row) {
      return tl::unexpected(row.error());
    }
    row->bind(1, month).bind(2, store::to_unix_seconds(report.generated_at));
    if (report.against) {
      row->bind(3, month_text(*report.against));
    } else {
      row->bind(3, nullptr);
    }
    return row->bind(4, utf8(report.markdown_file))
        .bind(5, utf8(report.html_file))
        .bind(6, report.markdown)
        .bind(7, report.diff.dump(-1, ' ', false, Json::error_handler_t::replace))
        .run();
  });
}

Result<std::optional<Json>> load_snapshot(store::Database& db, std::chrono::year_month month) {
  Result<store::Statement> query = db.prepare("SELECT data FROM snapshot WHERE month = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, month_text(month));
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return std::optional<Json>{};
  }
  return parse_column(query->column_text(0), "snapshot").transform([](Json value) {
    return std::optional<Json>(std::move(value));
  });
}

Result<std::optional<std::pair<std::chrono::year_month, Json>>> newest_before(
    store::Database& db, std::chrono::year_month month) {
  using Found = std::optional<std::pair<std::chrono::year_month, Json>>;
  Result<store::Statement> query =
      db.prepare("SELECT month, data FROM snapshot WHERE month < ?1 ORDER BY month DESC LIMIT 1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, month_text(month));
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return Found{};
  }
  Result<std::optional<std::chrono::year_month>> found = month_column(*query, 0);
  if (!found) {
    return tl::unexpected(found.error());
  }
  Result<Json> data = parse_column(query->column_text(1), "snapshot");
  if (!data) {
    return tl::unexpected(data.error());
  }
  return Found{std::pair(**found, std::move(*data))};
}

Result<std::optional<StoredReport>> load_report(store::Database& db,
                                                std::chrono::year_month month) {
  Result<store::Statement> query = db.prepare(
      "SELECT generated_at, against, markdown_path, html_path, markdown, diff FROM report "
      "WHERE month = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, month_text(month));
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return std::optional<StoredReport>{};
  }
  Result<std::optional<std::chrono::year_month>> against = month_column(*query, 1);
  if (!against) {
    return tl::unexpected(against.error());
  }
  Result<Json> diff = parse_column(query->column_text(5), "diff");
  if (!diff) {
    return tl::unexpected(diff.error());
  }
  return std::optional(StoredReport{.month = month,
                                    .generated_at = store::from_unix_seconds(query->column_int(0)),
                                    .against = *against,
                                    .markdown_file = from_utf8(query->column_text(2)),
                                    .html_file = from_utf8(query->column_text(3)),
                                    .markdown = query->column_text(4),
                                    .diff = std::move(*diff)});
}

Result<std::vector<std::chrono::year_month>> stored_months(store::Database& db, bool snapshots) {
  Result<store::Statement> query =
      db.prepare(snapshots ? "SELECT month FROM snapshot ORDER BY month DESC"
                           : "SELECT month FROM report ORDER BY month DESC");
  if (!query) {
    return tl::unexpected(query.error());
  }
  std::vector<std::chrono::year_month> months;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    Result<std::optional<std::chrono::year_month>> month = month_column(*query, 0);
    if (!month) {
      return tl::unexpected(month.error());
    }
    months.push_back(**month);
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return months;
}

}  // namespace myclean::report
