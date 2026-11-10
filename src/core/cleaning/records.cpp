#include "core/cleaning/records.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/audit_log.hpp"
#include "core/actions/policy.hpp"
#include "core/model/action.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::cleaning {

namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 11> kind_names = {
    "cleanup",     "handler", "native",    "relocate", "system_setting", "quarantine_restore",
    "recycle_bin", "archive", "uninstall", "compact",  "pause"};

std::string utf8(const std::filesystem::path& path) {
  return utf8_text(path);
}

std::filesystem::path from_utf8(const std::string& text) {
  return {std::u8string(text.begin(), text.end())};
}

constexpr std::string_view candidate_columns =
    "id, source, rule, category, tier, scope, strategy, path, size, command, handler, reason, "
    "found_at";

Result<Candidate> read_candidate(const store::Statement& row) {
  const std::optional<Tier> tier = parse_tier(row.column_text(4));
  const std::optional<ExecutionContext> scope = parse_execution_context(row.column_text(5));
  if (!tier || !scope) {
    return fail(Errc::invalid_data,
                "candidate " + std::to_string(row.column_int(0)) + " has an unknown tier or scope");
  }
  return Candidate{.id = row.column_int(0),
                   .source = row.column_text(1),
                   .rule = row.column_text(2),
                   .category = row.column_text(3),
                   .tier = *tier,
                   .scope = *scope,
                   .strategy = row.column_text(6),
                   .path = from_utf8(row.column_text(7)),
                   .size = Bytes{static_cast<std::uint64_t>(row.column_int(8))},
                   .command = row.column_text(9),
                   .handler = row.column_text(10),
                   .reason = row.column_text(11),
                   .found_at = store::from_unix_seconds(row.column_int(12))};
}

}  // namespace

const store::MigrationSet& cleaning_migrations() {
  static const store::MigrationSet set{
      .component = "cleaning",
      .migrations = {
          {.version = 1, .name = "candidates_and_notes", .sql = R"sql(
CREATE TABLE cleaning_candidate(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source TEXT NOT NULL,
  rule TEXT NOT NULL,
  category TEXT NOT NULL,
  tier TEXT NOT NULL,
  scope TEXT NOT NULL,
  strategy TEXT NOT NULL,
  path TEXT NOT NULL,
  size INTEGER NOT NULL,
  command TEXT NOT NULL,
  handler TEXT NOT NULL,
  reason TEXT NOT NULL,
  found_at INTEGER NOT NULL);
CREATE INDEX cleaning_candidate_source ON cleaning_candidate(source);
CREATE TABLE cleaning_item(
  item_id INTEGER PRIMARY KEY,
  rule TEXT NOT NULL,
  reason TEXT NOT NULL);
)sql"},
      }};
  return set;
}

std::string_view to_string(PlanKind kind) {
  return detail::enum_name(kind_names, kind);
}

std::optional<PlanKind> parse_plan_kind(std::string_view text) {
  return detail::enum_from_name<PlanKind>(kind_names, text);
}

std::string_view scope_name(ExecutionContext scope) {
  return scope == ExecutionContext::user ? "user" : "service";
}

std::string to_text(const PlanSpec& spec) {
  return Json{{"kind", to_string(spec.kind)},
              {"tier", to_string(spec.tier)},
              {"scope", to_string(spec.scope)},
              {"title", spec.title},
              {"group", spec.group},
              {"needs_restart", spec.needs_restart},
              {"commands", spec.commands},
              {"undo", spec.undo},
              {"warnings", spec.warnings},
              {"params", spec.params}}
      .dump(-1, ' ', false, Json::error_handler_t::replace);
}

Result<PlanSpec> parse_spec(std::string_view text) {
  const Json json = Json::parse(text, nullptr, false);
  try {
    if (!json.is_object()) {
      return fail(Errc::invalid_data, "plan spec is not a JSON object");
    }
    const std::optional<PlanKind> kind = parse_plan_kind(json.at("kind").get<std::string>());
    const std::optional<Tier> tier = parse_tier(json.at("tier").get<std::string>());
    const std::optional<ExecutionContext> scope =
        parse_execution_context(json.at("scope").get<std::string>());
    if (!kind || !tier || !scope) {
      return fail(Errc::invalid_data, "plan spec has an unknown kind, tier or scope");
    }
    return PlanSpec{.kind = *kind,
                    .tier = *tier,
                    .scope = *scope,
                    .title = json.at("title").get<std::string>(),
                    .group = json.at("group").get<std::string>(),
                    .needs_restart = json.at("needs_restart").get<bool>(),
                    .commands = json.at("commands").get<std::vector<std::string>>(),
                    .undo = json.at("undo").get<std::string>(),
                    .warnings = json.at("warnings").get<std::vector<std::string>>(),
                    .params = json.at("params")};
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("plan spec: ") + error.what());
  }
}

PlanSpec implicit_spec(std::string_view action) {
  if (action == "RestoreQuarantine") {
    return PlanSpec{.kind = PlanKind::quarantine_restore,
                    .tier = Tier::ask,
                    .title = "Restore quarantined items",
                    .undo = "the items can be quarantined again by a new plan"};
  }
  if (action == "PurgeQuarantine") {
    return PlanSpec{.kind = PlanKind::cleanup,
                    .tier = Tier::automatic,
                    .title = "Purge quarantine older than 7 days",
                    .undo = "not reversible: the quarantined items are deleted"};
  }
  return PlanSpec{.title = std::string(action)};
}

Result<PlanSpec> spec_of(store::Database& db, const Plan& plan) {
  Result<std::string> text = actions::load_params(db, plan.id);
  if (!text) {
    return tl::unexpected(text.error());
  }
  return text->empty() ? implicit_spec(plan.action) : parse_spec(*text);
}

Result<Plan> submit(actions::PolicyEngine& engine, store::Store& store, Plan plan,
                    const PlanSpec& spec, std::span<const ItemNote> notes,
                    actions::AutomationLevel level) {
  Result<Plan> stored = engine.submit(std::move(plan), level, to_text(spec));
  if (!stored || notes.empty()) {
    return stored;
  }
  const Result<void> written = store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> insert =
        db.prepare("INSERT INTO cleaning_item(item_id, rule, reason) VALUES(?1, ?2, ?3)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    for (std::size_t index = 0; index < notes.size() && index < stored->items.size(); ++index) {
      insert->bind(1, stored->items[index].id.value())
          .bind(2, notes[index].rule)
          .bind(3, notes[index].reason);
      if (Result<void> ran = insert->run(); !ran) {
        return ran;
      }
    }
    return {};
  });
  if (!written) {
    return tl::unexpected(written.error());
  }
  return stored;
}

Result<std::optional<ItemNote>> note_of(store::Database& db, PlanItemId item) {
  Result<store::Statement> query =
      db.prepare("SELECT rule, reason FROM cleaning_item WHERE item_id = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, item.value());
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return std::optional<ItemNote>{};
  }
  return ItemNote{.rule = query->column_text(0), .reason = query->column_text(1)};
}

Result<void> replace_candidates(store::Database& db, std::string_view source,
                                std::vector<Candidate>& candidates) {
  return db.transaction([&]() -> Result<void> {
    Result<store::Statement> remove =
        db.prepare("DELETE FROM cleaning_candidate WHERE source = ?1");
    if (!remove) {
      return tl::unexpected(remove.error());
    }
    if (Result<void> ran = remove->bind(1, source).run(); !ran) {
      return ran;
    }
    Result<store::Statement> insert = db.prepare(
        "INSERT INTO cleaning_candidate(source, rule, category, tier, scope, strategy, path, "
        "size, command, handler, reason, found_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    for (Candidate& candidate : candidates) {
      candidate.source = std::string(source);
      insert->bind(1, source)
          .bind(2, candidate.rule)
          .bind(3, candidate.category)
          .bind(4, to_string(candidate.tier))
          .bind(5, to_string(candidate.scope))
          .bind(6, candidate.strategy)
          .bind(7, utf8(candidate.path))
          .bind(8, candidate.size.count())
          .bind(9, candidate.command)
          .bind(10, candidate.handler)
          .bind(11, candidate.reason)
          .bind(12, store::to_unix_seconds(candidate.found_at));
      if (Result<void> ran = insert->run(); !ran) {
        return ran;
      }
      candidate.id = db.last_insert_id();
    }
    return {};
  });
}

Result<std::vector<Candidate>> candidates_by_id(store::Database& db,
                                                std::span<const std::int64_t> ids) {
  Result<store::Statement> query = db.prepare("SELECT " + std::string(candidate_columns) +
                                              " FROM cleaning_candidate WHERE id = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  std::vector<Candidate> found;
  for (const std::int64_t id : ids) {
    query->bind(1, id);
    const Result<bool> row = query->step();
    if (!row) {
      return tl::unexpected(row.error());
    }
    if (!*row) {
      return fail(Errc::not_found,
                  "no candidate " + std::to_string(id) + " (unknown, or replaced by a newer scan)");
    }
    Result<Candidate> candidate = read_candidate(*query);
    if (!candidate) {
      return tl::unexpected(candidate.error());
    }
    found.push_back(*std::move(candidate));
    if (Result<void> reset = query->run(); !reset) {
      return tl::unexpected(reset.error());
    }
  }
  return found;
}

Result<CandidateTotals> candidate_totals(store::Database& db, std::string_view source) {
  Result<store::Statement> query = db.prepare(
      "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM cleaning_candidate WHERE source = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, source);
  if (Result<bool> row = query->step(); !row) {
    return tl::unexpected(row.error());
  }
  return CandidateTotals{.count = query->column_int(0),
                         .bytes = Bytes{static_cast<std::uint64_t>(query->column_int(1))}};
}

}  // namespace myclean::cleaning
