#include "core/startup/boot_times.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/impact.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

namespace myclean::startup {
namespace {

// The rows `sql` (its parameters set by `bind`) returns, each read with `row_of`.
template <typename T, typename Bind, typename RowOf>
Result<std::vector<T>> query(store::Store& store, std::string_view sql, Bind bind, RowOf row_of) {
  std::vector<T> rows;
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(sql);
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    bind(*statement);
    for (Result<bool> row = statement->step();; row = statement->step()) {
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      rows.push_back(row_of(*statement));
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return rows;
}

std::chrono::milliseconds median_ms(std::vector<std::chrono::milliseconds> values) {
  std::ranges::sort(values);
  const std::size_t middle = values.size() / 2;
  return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

}  // namespace

Result<void> record_boots(store::Store& store, std::span<const BootRecord> boots) {
  return store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> insert = db.prepare(
        "INSERT OR REPLACE INTO boot_time(start, boot_ms, main_path_ms, post_boot_ms, logon_ms) "
        "VALUES (?1, ?2, ?3, ?4, ?5)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    for (const BootRecord& boot : boots) {
      Result<void> inserted = insert->bind(1, store::to_unix_seconds(boot.start))
                                  .bind(2, boot.boot.count())
                                  .bind(3, boot.main_path.count())
                                  .bind(4, boot.post_boot.count())
                                  .bind(5, boot.logon.count())
                                  .run();
      if (!inserted) {
        return inserted;
      }
    }
    return {};
  });
}

Result<void> record_boot_events(store::Store& store, const IDiagnosticsLog& log) {
  const Result<std::vector<std::string>> events = log.events();
  if (!events) {
    return tl::unexpected(events.error());
  }
  return record_boots(store, boot_records(*events));
}

Result<std::vector<BootRecord>> stored_boots(store::Store& store) {
  return query<BootRecord>(
      store,
      "SELECT start, boot_ms, main_path_ms, post_boot_ms, logon_ms FROM boot_time ORDER BY start",
      [](store::Statement& /*statement*/) {},
      [](const store::Statement& row) {
        return BootRecord{.start = store::from_unix_seconds(row.column_int(0)),
                          .boot = std::chrono::milliseconds{row.column_int(1)},
                          .main_path = std::chrono::milliseconds{row.column_int(2)},
                          .post_boot = std::chrono::milliseconds{row.column_int(3)},
                          .logon = std::chrono::milliseconds{row.column_int(4)}};
      });
}

Result<std::vector<WallTime>> stored_logons(store::Store& store) {
  return query<WallTime>(
      store, "SELECT logon FROM logon_recorded ORDER BY logon",
      [](store::Statement& /*statement*/) {},
      [](const store::Statement& row) { return store::from_unix_seconds(row.column_int(0)); });
}

Result<std::vector<ImageImpact>> stored_logon_impact(store::Store& store, WallTime logon) {
  return query<ImageImpact>(
      store,
      "SELECT image, cpu_seconds, io_bytes FROM logon_impact WHERE logon = ?1 ORDER BY image",
      [logon](store::Statement& statement) { statement.bind(1, store::to_unix_seconds(logon)); },
      [](const store::Statement& row) {
        return ImageImpact{.image = row.column_text(0),
                           .cpu_seconds = row.column_double(1),
                           .io_bytes = static_cast<std::uint64_t>(row.column_int(2))};
      });
}

std::optional<WallTime> boot_logon(std::span<const BootRecord> boots, std::size_t index,
                                   std::span<const WallTime> logons) {
  if (index >= boots.size()) {
    return std::nullopt;
  }
  const WallTime start = boots[index].start;
  const auto first = std::ranges::lower_bound(logons, start);
  if (first == logons.end() || (index + 1 < boots.size() && *first >= boots[index + 1].start)) {
    return std::nullopt;
  }
  return *first;
}

std::vector<ImageImpact> by_impact(std::vector<ImageImpact> impact) {
  std::ranges::sort(impact, [](const ImageImpact& left, const ImageImpact& right) {
    return std::tie(right.io_bytes, right.cpu_seconds, left.image) <
           std::tie(left.io_bytes, left.cpu_seconds, right.image);
  });
  return impact;
}

std::optional<DurationMedian> median_of(std::span<const BootRecord> boots) {
  if (boots.empty()) {
    return std::nullopt;
  }
  std::vector<std::chrono::milliseconds> boot;
  std::vector<std::chrono::milliseconds> logon;
  for (const BootRecord& record : boots) {
    boot.push_back(record.boot);
    logon.push_back(record.logon);
  }
  return DurationMedian{.boot = median_ms(std::move(boot)),
                        .logon = median_ms(std::move(logon)),
                        .boots = boots.size()};
}

std::span<const BootRecord> boots_before(std::span<const BootRecord> boots, WallTime when,
                                         std::size_t count) {
  const auto end = std::ranges::lower_bound(boots, when, {}, &BootRecord::start);
  const auto size = static_cast<std::size_t>(end - boots.begin());
  const std::size_t taken = std::min(size, count);
  return boots.subspan(size - taken, taken);
}

std::span<const BootRecord> boots_after(std::span<const BootRecord> boots, WallTime when,
                                        std::size_t count) {
  const auto begin = std::ranges::lower_bound(boots, when, {}, &BootRecord::start);
  const auto offset = static_cast<std::size_t>(begin - boots.begin());
  return boots.subspan(offset, std::min(boots.size() - offset, count));
}

}  // namespace myclean::startup
