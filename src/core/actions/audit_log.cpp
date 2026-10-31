#include "core/actions/audit_log.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {
namespace {

void bind_state(store::Statement& statement, int index, std::optional<PlanState> state) {
  if (state) {
    statement.bind(index, to_string(*state));
  } else {
    statement.bind(index, nullptr);
  }
}

std::optional<PlanState> column_state(const store::Statement& statement, int column) {
  if (statement.is_null(column)) {
    return std::nullopt;
  }
  return parse_plan_state(statement.column_text(column));
}

}  // namespace

Result<std::int64_t> append_log(store::Database& db, const LogEntry& entry) {
  Result<store::Statement> insert = db.prepare(
      "INSERT INTO action_log(time, plan_id, item_id, actor, event, from_state, to_state, "
      "outcome, reason, detail, metrics) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  insert->bind(1, store::to_unix_seconds(entry.time))
      .bind(2, entry.plan.value())
      .bind(4, entry.actor)
      .bind(5, entry.event)
      .bind(8, entry.outcome)
      .bind(9, entry.reason)
      .bind(10, entry.detail)
      .bind(11, entry.metrics);
  if (entry.item) {
    insert->bind(3, entry.item->value());
  } else {
    insert->bind(3, nullptr);
  }
  bind_state(*insert, 6, entry.from);
  bind_state(*insert, 7, entry.to);
  if (Result<void> ran = insert->run(); !ran) {
    return tl::unexpected(ran.error());
  }
  return db.last_insert_id();
}

Result<std::vector<LogEntry>> plan_log(store::Database& db, PlanId plan) {
  Result<store::Statement> query = db.prepare(
      "SELECT time, item_id, actor, event, from_state, to_state, outcome, reason, detail, "
      "metrics FROM action_log WHERE plan_id = ?1 ORDER BY id");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, plan.value());
  std::vector<LogEntry> entries;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    entries.push_back(LogEntry{
        .time = store::from_unix_seconds(query->column_int(0)),
        .plan = plan,
        .item = query->is_null(1) ? std::nullopt
                                  : std::optional<PlanItemId>(PlanItemId{query->column_int(1)}),
        .actor = query->column_text(2),
        .event = query->column_text(3),
        .from = column_state(*query, 4),
        .to = column_state(*query, 5),
        .outcome = query->column_text(6),
        .reason = query->column_text(7),
        .detail = query->column_text(8),
        .metrics = query->column_text(9)});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return entries;
}

Result<void> save_undo(store::Database& db, std::int64_t log_id, PlanId plan, const Undo& undo) {
  Result<store::Statement> insert = db.prepare(
      "INSERT INTO undo_blob(log_id, plan_id, item_id, data, not_reversible) "
      "VALUES(?1, ?2, ?3, ?4, ?5)");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  // A row without an item carries the plan-wide part: why the change is not reversible.
  insert->bind(1, log_id)
      .bind(2, plan.value())
      .bind(3, nullptr)
      .bind(4, std::span<const std::uint8_t>{})
      .bind(5, undo.not_reversible);
  if (Result<void> ran = insert->run(); !ran) {
    return ran;
  }
  for (const UndoItem& item : undo.items) {
    const std::vector<std::uint8_t> data(item.data.begin(), item.data.end());
    insert->bind(1, log_id)
        .bind(2, plan.value())
        .bind(3, item.item.value())
        .bind(4, std::span<const std::uint8_t>(data))
        .bind(5, "");
    if (Result<void> ran = insert->run(); !ran) {
      return ran;
    }
  }
  return {};
}

Result<Undo> load_undo(store::Database& db, PlanId plan) {
  Result<store::Statement> query = db.prepare(
      "SELECT item_id, data, not_reversible FROM undo_blob WHERE plan_id = ?1 ORDER BY id");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, plan.value());
  std::optional<Undo> undo;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    if (!undo) {
      undo.emplace();
    }
    if (query->is_null(0)) {
      undo->not_reversible = query->column_text(2);
      continue;
    }
    const std::vector<std::uint8_t> data = query->column_blob(1);
    undo->items.push_back(
        UndoItem{.item = PlanItemId{query->column_int(0)}, .data = {data.begin(), data.end()}});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!undo) {
    return fail(Errc::not_found, "no undo data for plan " + std::to_string(plan.value()));
  }
  return *std::move(undo);
}

}  // namespace myclean::actions
