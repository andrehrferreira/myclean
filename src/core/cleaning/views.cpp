#include "core/cleaning/views.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/audit_log.hpp"
#include "core/actions/quarantine.hpp"
#include "core/cleaning/records.hpp"
#include "core/ipc/plans.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/startup/toggle.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

namespace myclean::cleaning {

namespace {

using Json = nlohmann::json;

std::string utf8(const std::filesystem::path& path) {
  return utf8_text(path);
}

Json summary(std::int64_t id, const PlanSpec& spec, const std::string& proposer, PlanState state,
             WallTime created_at, std::int64_t items, std::uint64_t bytes) {
  return Json{{"id", id},
              {"title", spec.title},
              {"kind", to_string(spec.kind)},
              {"tier", to_string(spec.tier)},
              {"scope", scope_name(spec.scope)},
              {"state", to_string(state)},
              {"proposer", proposer},
              {"created_at", store::to_unix_seconds(created_at)},
              {"items", items},
              {"bytes", bytes},
              {"needs_restart", spec.needs_restart}};
}

// One PlanItem of plans.get.
Result<Json> item_json(store::Database& db, const PlanItem& item) {
  const Result<std::optional<ItemNote>> note = note_of(db, item.id);
  if (!note) {
    return tl::unexpected(note.error());
  }
  const ItemNote known = note->value_or(ItemNote{});
  Json approved = nullptr;
  if (item.approval != ItemApproval::pending) {
    approved = item.approval == ItemApproval::approved;
  }
  return Json{
      {"id", item.id.value()},      {"path", utf8(item.path)},
      {"bytes", item.size.count()}, {"rule", known.rule.empty() ? Json(nullptr) : Json(known.rule)},
      {"reason", known.reason},     {"approved", std::move(approved)}};
}

// The "freed" and "detail" members of plans.get, from the plan's log.
std::pair<Json, Json> outcome_of(const std::vector<actions::LogEntry>& log) {
  constexpr std::string_view prefix = "freed=";
  std::optional<std::uint64_t> freed;
  Json detail = nullptr;
  for (const actions::LogEntry& entry : log) {
    if (entry.event == "apply" && entry.metrics.starts_with(prefix)) {
      freed = freed.value_or(0) + std::stoull(entry.metrics.substr(prefix.size()));
    }
    const bool ended = entry.to == PlanState::failed || entry.to == PlanState::rejected ||
                       entry.to == PlanState::rolled_back;
    if (entry.event != "transition" || entry.outcome != "accepted" || !ended) {
      continue;
    }
    std::string text = entry.reason;
    if (!entry.detail.empty()) {
      text += (text.empty() ? "" : ": ") + entry.detail;
    }
    detail = text.empty() ? Json(nullptr) : Json(text);
  }
  return {freed ? Json(*freed) : Json(nullptr), std::move(detail)};
}

Result<Json> plan_json(store::Database& db, const Plan& plan) {
  Result<PlanSpec> spec = spec_of(db, plan);
  if (!spec) {
    return tl::unexpected(spec.error());
  }
  Bytes total;
  Json items = Json::array();
  for (const PlanItem& item : plan.items) {
    total += item.size;
    if (items.size() < ipc::max_plan_items) {
      Result<Json> json = item_json(db, item);
      if (!json) {
        return json;
      }
      items.push_back(*std::move(json));
    }
  }
  Result<std::vector<actions::LogEntry>> log = actions::plan_log(db, plan.id);
  if (!log) {
    return tl::unexpected(log.error());
  }
  auto [freed, detail] = outcome_of(*log);
  Json result =
      summary(plan.id.value(), *spec, plan.proposer.to_string(), plan.state, plan.created_at,
              static_cast<std::int64_t>(plan.items.size()), total.count());
  result["items"] = std::move(items);
  result["items_total"] = plan.items.size();
  result["commands"] = spec->commands;
  result["undo"] = spec->undo;
  result["warnings"] = spec->warnings;
  result["params"] = spec->params;
  result["freed"] = std::move(freed);
  result["detail"] = std::move(detail);
  return result;
}

}  // namespace

Result<Json> list_plans(store::Store& store, std::optional<PlanState> state) {
  Json plans = Json::array();
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(
        "SELECT p.id, p.action, p.proposer, p.state, p.created_at, "
        "(SELECT COUNT(*) FROM plan_item i WHERE i.plan_id = p.id), "
        "(SELECT COALESCE(SUM(i.size), 0) FROM plan_item i WHERE i.plan_id = p.id), "
        "COALESCE(pp.params, '') FROM plan p LEFT JOIN plan_params pp ON pp.plan_id = p.id "
        "WHERE ?1 IS NULL OR p.state = ?1 ORDER BY p.id DESC LIMIT ?2");
    if (!query) {
      return tl::unexpected(query.error());
    }
    if (state) {
      query->bind(1, to_string(*state));
    } else {
      query->bind(1, nullptr);
    }
    query->bind(2, static_cast<std::int64_t>(ipc::max_listed_plans));
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      const std::optional<PlanState> plan_state = parse_plan_state(query->column_text(3));
      if (!plan_state) {
        continue;
      }
      const std::string params = query->column_text(7);
      Result<PlanSpec> spec =
          params.empty() ? implicit_spec(query->column_text(1)) : parse_spec(params);
      if (!spec) {
        spec = implicit_spec(query->column_text(1));
      }
      plans.push_back(summary(query->column_int(0), *spec, query->column_text(2), *plan_state,
                              store::from_unix_seconds(query->column_int(4)), query->column_int(5),
                              static_cast<std::uint64_t>(query->column_int(6))));
    }
    return row ? Result<void>{} : tl::unexpected(row.error());
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return Json{{"plans", std::move(plans)}};
}

Result<Json> get_plan(store::Store& store, PlanId id) {
  Result<Json> answer = fail(Errc::not_found, "plan not read");
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    const Result<Plan> plan = store::get_plan(db, id);
    if (plan) {
      answer = plan_json(db, *plan);
    } else {
      answer = tl::unexpected(plan.error());
    }
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return answer;
}

Result<std::vector<StartupChange>> startup_changes(store::Store& store) {
  std::vector<StartupChange> changes;
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(
        "SELECT p.id, pp.params, (SELECT MIN(a.time) FROM action_log a WHERE a.plan_id = p.id "
        "AND a.to_state = 'applied') FROM plan p JOIN plan_params pp ON pp.plan_id = p.id "
        "WHERE p.action = ?1 AND p.state IN ('applied', 'verified') ORDER BY p.id DESC LIMIT ?2");
    if (!query) {
      return tl::unexpected(query.error());
    }
    query->bind(1, startup::toggle_action_name)
        .bind(2, static_cast<std::int64_t>(ipc::max_listed_plans));
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      const Result<PlanSpec> spec = parse_spec(query->column_text(1));
      if (!spec || query->is_null(2)) {
        continue;
      }
      const Json& items = spec->params.contains("items") ? spec->params.at("items") : Json();
      if (!items.is_array()) {
        continue;
      }
      StartupChange change{.plan = PlanId{query->column_int(0)},
                           .applied = store::from_unix_seconds(query->column_int(2)),
                           .enable = spec->params.value("enable", false)};
      for (const Json& item : items) {
        if (item.is_string()) {
          change.items.push_back(item.get<std::string>());
        }
      }
      changes.push_back(std::move(change));
    }
    return row ? Result<void>{} : tl::unexpected(row.error());
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return changes;
}

Result<Json> list_quarantine(store::Store& store) {
  Result<std::vector<actions::QuarantineRecord>> held = fail(Errc::io_error, "not read");
  if (const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
        held = actions::held_quarantine(db);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!held) {
    return tl::unexpected(held.error());
  }
  std::map<std::int64_t, Json> runs;
  std::size_t listed = 0;
  for (const actions::QuarantineRecord& record : *held) {
    auto [entry, added] = runs.try_emplace(record.run.value());
    Json& run = entry->second;
    const std::int64_t moved = store::to_unix_seconds(record.moved_at);
    const std::int64_t purge = store::to_unix_seconds(record.purge_after);
    if (added) {
      run = Json{{"plan", record.run.value()},
                 {"moved_at", moved},
                 {"purge_after", purge},
                 {"bytes", 0},
                 {"items", Json::array()}};
    }
    run["moved_at"] = std::min(run["moved_at"].get<std::int64_t>(), moved);
    run["purge_after"] = std::max(run["purge_after"].get<std::int64_t>(), purge);
    run["bytes"] = run["bytes"].get<std::uint64_t>() + record.size.count();
    if (listed++ < ipc::max_plan_items) {
      run["items"].push_back(Json{{"item", record.item.value()},
                                  {"original", utf8(record.original)},
                                  {"location", utf8(record.location)},
                                  {"bytes", record.size.count()}});
    }
  }
  Json list = Json::array();
  for (auto& [run, json] : runs) {
    list.push_back(std::move(json));
  }
  return Json{{"runs", std::move(list)}};
}

}  // namespace myclean::cleaning
