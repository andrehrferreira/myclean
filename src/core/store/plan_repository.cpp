#include "core/store/plan_repository.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/finding.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"

namespace myclean::store {
namespace {

Result<void> expect_one_change(const Database& db, std::string_view what, std::int64_t id) {
  if (db.changes() == 0) {
    return fail(Errc::not_found, "no " + std::string(what) + " with id " + std::to_string(id));
  }
  return {};
}

Result<std::vector<PlanItem>> get_items(Database& db, PlanId plan) {
  Result<Statement> query = db.prepare(
      "SELECT id, path, volume_serial, file_id, size, approval FROM plan_item "
      "WHERE plan_id = ?1 ORDER BY id");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, plan.value());
  std::vector<PlanItem> items;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    const std::string path = query->column_text(1);
    const std::vector<std::uint8_t> file_id = query->column_blob(3);
    const std::optional<ItemApproval> approval = parse_item_approval(query->column_text(5));
    FileIdentity identity{.volume_serial = static_cast<std::uint64_t>(query->column_int(2))};
    if (file_id.size() != identity.file_id.size() || !approval) {
      return fail(Errc::invalid_data, "plan_item " + std::to_string(query->column_int(0)) +
                                          " has a malformed file id or approval");
    }
    std::ranges::copy(file_id, identity.file_id.begin());
    items.push_back(PlanItem{.id = PlanItemId{query->column_int(0)},
                             .path = std::filesystem::path(std::u8string(path.begin(), path.end())),
                             .identity = identity,
                             .size = Bytes{static_cast<std::uint64_t>(query->column_int(4))},
                             .approval = *approval});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return items;
}

}  // namespace

Result<FindingId> add_finding(Database& db, const Finding& finding) {
  Result<Statement> insert = db.prepare(
      "INSERT INTO finding(kind, subject, severity, summary, detected_at) "
      "VALUES(?1, ?2, ?3, ?4, ?5)");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  insert->bind(1, finding.kind)
      .bind(2, finding.subject)
      .bind(3, to_string(finding.severity))
      .bind(4, finding.summary)
      .bind(5, to_unix_seconds(finding.detected_at));
  if (Result<void> ran = insert->run(); !ran) {
    return tl::unexpected(ran.error());
  }
  return FindingId{db.last_insert_id()};
}

Result<Finding> get_finding(Database& db, FindingId id) {
  Result<Statement> query =
      db.prepare("SELECT kind, subject, severity, summary, detected_at FROM finding WHERE id = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, id.value());
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return fail(Errc::not_found, "no finding with id " + std::to_string(id.value()));
  }
  const std::optional<Severity> severity = parse_severity(query->column_text(2));
  if (!severity) {
    return fail(Errc::invalid_data,
                "finding " + std::to_string(id.value()) + " has an unknown severity");
  }
  return Finding{.id = id,
                 .kind = query->column_text(0),
                 .subject = query->column_text(1),
                 .severity = *severity,
                 .summary = query->column_text(3),
                 .detected_at = from_unix_seconds(query->column_int(4))};
}

Result<Plan> add_plan(Database& db, Plan plan) {
  const Result<void> stored = db.transaction([&]() -> Result<void> {
    Result<Statement> insert = db.prepare(
        "INSERT INTO plan(action, finding_id, proposer, state, created_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    insert->bind(1, plan.action)
        .bind(3, plan.proposer.to_string())
        .bind(4, to_string(plan.state))
        .bind(5, to_unix_seconds(plan.created_at));
    if (plan.finding) {
      insert->bind(2, plan.finding->value());
    } else {
      insert->bind(2, nullptr);
    }
    if (Result<void> ran = insert->run(); !ran) {
      return ran;
    }
    plan.id = PlanId{db.last_insert_id()};

    Result<Statement> insert_item = db.prepare(
        "INSERT INTO plan_item(plan_id, path, volume_serial, file_id, size, approval) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
    if (!insert_item) {
      return tl::unexpected(insert_item.error());
    }
    for (PlanItem& item : plan.items) {
      const std::u8string path = item.path.u8string();
      insert_item->bind(1, plan.id.value())
          .bind(2, std::string(path.begin(), path.end()))
          .bind(3, item.identity.volume_serial)
          .bind(4, std::span<const std::uint8_t>(item.identity.file_id))
          .bind(5, item.size.count())
          .bind(6, to_string(item.approval));
      if (Result<void> ran = insert_item->run(); !ran) {
        return ran;
      }
      item.id = PlanItemId{db.last_insert_id()};
    }
    return {};
  });
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  return plan;
}

Result<Plan> get_plan(Database& db, PlanId id) {
  Result<Statement> query =
      db.prepare("SELECT action, finding_id, proposer, state, created_at FROM plan WHERE id = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, id.value());
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return fail(Errc::not_found, "no plan with id " + std::to_string(id.value()));
  }
  Result<Actor> proposer = Actor::parse(query->column_text(2));
  const std::optional<PlanState> state = parse_plan_state(query->column_text(3));
  if (!proposer || !state) {
    return fail(Errc::invalid_data,
                "plan " + std::to_string(id.value()) + " has an unknown proposer or state");
  }
  Result<std::vector<PlanItem>> items = get_items(db, id);
  if (!items) {
    return tl::unexpected(items.error());
  }
  return Plan{.id = id,
              .action = query->column_text(0),
              .finding = query->is_null(1)
                             ? std::nullopt
                             : std::optional<FindingId>(FindingId{query->column_int(1)}),
              .proposer = std::move(*proposer),
              .state = *state,
              .created_at = from_unix_seconds(query->column_int(4)),
              .items = std::move(*items)};
}

Result<void> set_plan_state(Database& db, PlanId id, PlanState state) {
  Result<Statement> update = db.prepare("UPDATE plan SET state = ?2 WHERE id = ?1");
  if (!update) {
    return tl::unexpected(update.error());
  }
  if (Result<void> ran = update->bind(1, id.value()).bind(2, to_string(state)).run(); !ran) {
    return ran;
  }
  return expect_one_change(db, "plan", id.value());
}

Result<void> set_item_approval(Database& db, PlanItemId id, ItemApproval approval) {
  Result<Statement> update = db.prepare("UPDATE plan_item SET approval = ?2 WHERE id = ?1");
  if (!update) {
    return tl::unexpected(update.error());
  }
  if (Result<void> ran = update->bind(1, id.value()).bind(2, to_string(approval)).run(); !ran) {
    return ran;
  }
  return expect_one_change(db, "plan item", id.value());
}

}  // namespace myclean::store
