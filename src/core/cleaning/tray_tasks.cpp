#include "core/cleaning/tray_tasks.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/audit_log.hpp"
#include "core/actions/policy.hpp"
#include "core/cleaning/records.hpp"
#include "core/ipc/plans.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

namespace myclean::cleaning {

namespace {

using Json = nlohmann::json;

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

std::string text_of(const Json& result, const char* member) {
  const auto found = result.find(member);
  return found != result.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// What the tray reported, as the action the policy engine runs: apply returns the reported
// undo data (or the reported failure), verify the reported verification, undo the reported
// rollback.
class ReportedAction final : public Action {
 public:
  ReportedAction(Plan plan, const Json& result, std::string title)
      : plan_(std::move(plan)),
        result_(&result),
        state_(text_of(result, "state")),
        error_(text_of(result, "error")),
        title_(std::move(title)) {}

  [[nodiscard]] ActionInfo describe() const override {
    return {.what = title_, .why = "reported by the tray", .context = ExecutionContext::user};
  }

  [[nodiscard]] Result<Preview> preview() override {
    Preview preview;
    for (const PlanItem& item : plan_.items) {
      if (item.approval != ItemApproval::not_approved) {
        preview.items.push_back(item);
      }
    }
    return preview;
  }

  [[nodiscard]] Result<Undo> apply(const Preview& /*preview*/) override {
    if (state_ == "failed") {
      return fail(Errc::io_error, error_.empty() ? "the tray reported a failure" : error_);
    }
    Undo undo{.not_reversible = text_of(*result_, "not_reversible")};
    if (const auto entries = result_->find("undo"); entries != result_->end()) {
      for (const Json& entry : *entries) {
        const PlanItemId item{entry.at("item").get<std::int64_t>()};
        const auto& data = entry.at("data").get_ref<const std::string&>();
        if (!undo.items.empty() && undo.items.back().item == item) {
          undo.items.back().data += data;  // a piece of a longer value
        } else {
          undo.items.push_back(UndoItem{.item = item, .data = data});
        }
      }
    }
    return undo;
  }

  [[nodiscard]] Result<Verification> verify(const Undo& /*undo*/) override {
    const auto verification = result_->find("verification");
    if (verification != result_->end() && !verification->is_null()) {
      const std::string effect = verification->at("effect").get<std::string>();
      return Verification{.metric = verification->at("metric").get<std::string>(),
                          .before = verification->at("before").get<double>(),
                          .after = verification->at("after").get<double>(),
                          .effect = parse_effect(effect).value_or(Effect::worse)};
    }
    if (state_ == "rolled_back") {
      return Verification{.metric = "reported", .effect = Effect::worse};
    }
    if (state_ == "applied") {
      return fail(Errc::io_error, error_.empty() ? "the tray could not verify it" : error_);
    }
    return Verification{.metric = "reported", .effect = Effect::unchanged};
  }

  [[nodiscard]] Result<void> undo(const Undo& /*undo*/) override {
    if (state_ == "rolled_back") {
      return {};
    }
    return fail(Errc::io_error, error_.empty() ? "the tray could not undo it" : error_);
  }

 private:
  Plan plan_;
  const Json* result_;
  std::string state_;
  std::string error_;
  std::string title_;
};

std::string hex(const FileIdentity& identity) {
  std::string text;
  for (const std::uint8_t byte : identity.file_id) {
    text += std::format("{:02x}", byte);
  }
  return text;
}

Json item_json(const PlanItem& item) {
  return Json{{"id", item.id.value()},
              {"path", utf8(item.path)},
              {"bytes", item.size.count()},
              {"approved", item.approval == ItemApproval::pending
                               ? Json(nullptr)
                               : Json(item.approval == ItemApproval::approved)},
              {"volume_serial", item.identity.volume_serial},
              {"file_id", hex(item.identity)}};
}

struct Loaded {
  Plan plan;
  PlanSpec spec;
};

Result<Loaded> load(store::Store& store, PlanId id) {
  Result<Loaded> loaded = fail(Errc::not_found, "plan not read");
  if (Result<void> read = store.read([&](store::Database& db) -> Result<void> {
        Result<Plan> plan = store::get_plan(db, id);
        if (!plan) {
          loaded = tl::unexpected(plan.error());
          return {};
        }
        Result<PlanSpec> spec = spec_of(db, *plan);
        if (!spec) {
          loaded = tl::unexpected(spec.error());
          return {};
        }
        loaded = Loaded{.plan = *std::move(plan), .spec = *std::move(spec)};
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  return loaded;
}

}  // namespace

Result<Json> TrayTasks::task(PlanId id, bool undo) {
  Result<Loaded> loaded = load(*store_, id);
  if (!loaded) {
    return tl::unexpected(loaded.error());
  }
  const Plan& plan = loaded->plan;
  Json items = Json::array();
  for (const PlanItem& item : plan.items) {
    if (undo || item.approval != ItemApproval::not_approved) {
      items.push_back(item_json(item));
    }
  }
  Json task{{"plan", plan.id.value()},
            {"title", loaded->spec.title},
            {"kind", to_string(loaded->spec.kind)},
            {"operation", undo ? "undo" : "apply"},
            {"tier", to_string(loaded->spec.tier)},
            {"items", std::move(items)},
            {"action", loaded->spec.params}};
  if (undo) {
    Result<Undo> stored = fail(Errc::not_found, "not read");
    if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
          stored = actions::load_undo(db, id);
          return {};
        });
        !read) {
      return tl::unexpected(read.error());
    }
    if (!stored) {
      return tl::unexpected(stored.error());
    }
    Json entries = Json::array();
    for (const UndoItem& item : stored->items) {
      entries.push_back(Json{{"item", item.item.value()}, {"data", item.data}});
    }
    task["undo"] = Json{{"not_reversible", stored->not_reversible}, {"items", std::move(entries)}};
  }
  return task;
}

Result<Json> TrayTasks::next() {
  const std::scoped_lock lock(mutex_);
  const WallTime now = clock_->now();
  const auto free = [&](std::int64_t plan) {
    const auto claim = claims_.find(plan);
    return claim == claims_.end() ||
           now - claim->second >= std::chrono::minutes{ipc::task_claim_minutes};
  };
  for (const std::int64_t plan : undos_) {
    if (free(plan)) {
      Result<Json> answer = task(PlanId{plan}, true);
      if (!answer) {
        return tl::unexpected(answer.error());
      }
      claims_[plan] = now;
      return Json{{"task", *std::move(answer)}};
    }
  }
  std::vector<std::int64_t> approved;
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        Result<store::Statement> query = db.prepare(
            "SELECT p.id FROM plan p JOIN plan_params pp ON pp.plan_id = p.id "
            "WHERE p.state = 'approved' ORDER BY p.id");
        if (!query) {
          return tl::unexpected(query.error());
        }
        Result<bool> row = query->step();
        for (; row && *row; row = query->step()) {
          approved.push_back(query->column_int(0));
        }
        return row ? Result<void>{} : tl::unexpected(row.error());
      });
      !read) {
    return tl::unexpected(read.error());
  }
  for (const std::int64_t plan : approved) {
    if (!free(plan)) {
      continue;
    }
    Result<Loaded> loaded = load(*store_, PlanId{plan});
    if (!loaded || loaded->spec.scope != ExecutionContext::user) {
      continue;
    }
    Result<Json> answer = task(PlanId{plan}, false);
    if (!answer) {
      return tl::unexpected(answer.error());
    }
    claims_[plan] = now;
    return Json{{"task", *std::move(answer)}};
  }
  return Json{{"task", nullptr}};
}

Result<PlanState> TrayTasks::report(PlanId id, const Json& result) {
  Result<Loaded> loaded = load(*store_, id);
  if (!loaded) {
    return tl::unexpected(loaded.error());
  }
  if (loaded->spec.scope != ExecutionContext::user) {
    return fail(Errc::invalid_state, "plan " + std::to_string(id.value()) + " is not a user task");
  }
  const PlanState state = loaded->plan.state;
  bool queued_undo = false;
  {
    const std::scoped_lock lock(mutex_);
    queued_undo = undos_.contains(id.value());
  }
  const bool apply = state == PlanState::approved;
  const bool applied = state == PlanState::applied || state == PlanState::verified;
  const bool undo = queued_undo && applied;
  if (!apply && !undo) {
    return fail(Errc::invalid_state, "plan " + std::to_string(id.value()) + " (" +
                                         std::string(to_string(state)) + ") has no task to report");
  }
  const std::string reported = text_of(result, "state");
  ReportedAction action(loaded->plan, result, loaded->spec.title);
  const Error* failure = nullptr;
  Result<actions::RunReport> ran = actions::RunReport{};
  Result<void> undone;
  if (apply) {
    ran = engine_->execute(id, action, Actor::owner(), reported == "rolled_back");
    failure = ran ? nullptr : &ran.error();
  } else {
    undone = engine_->undo(id, action, Actor::owner());
    failure = undone ? nullptr : &undone.error();
  }
  {
    const std::scoped_lock lock(mutex_);
    claims_.erase(id.value());
    undos_.erase(id.value());
  }
  if (failure != nullptr && reported != "failed") {
    return tl::unexpected(*failure);
  }

  const auto skipped = result.find("skipped");
  if (Result<void> logged = store_->write([&](store::Database& db) -> Result<void> {
        if (skipped == result.end()) {
          return {};
        }
        for (const Json& entry : *skipped) {
          const Result<std::int64_t> row = actions::append_log(
              db, actions::LogEntry{.time = clock_->now(),
                                    .plan = id,
                                    .actor = Actor::owner().to_string(),
                                    .event = "apply",
                                    .outcome = "skipped",
                                    .reason = entry.at("code").get<std::string>(),
                                    .detail = entry.at("target").get<std::string>() + ": " +
                                              entry.at("message").get<std::string>()});
          if (!row) {
            return tl::unexpected(row.error());
          }
        }
        return {};
      });
      !logged) {
    return tl::unexpected(logged.error());
  }
  Result<Loaded> after = load(*store_, id);
  if (!after) {
    return tl::unexpected(after.error());
  }
  return after->plan.state;
}

Result<PlanState> TrayTasks::queue_undo(PlanId id) {
  Result<Loaded> loaded = load(*store_, id);
  if (!loaded) {
    return tl::unexpected(loaded.error());
  }
  const PlanState state = loaded->plan.state;
  const bool applied = state == PlanState::applied || state == PlanState::verified;
  if (loaded->spec.scope != ExecutionContext::user || !applied) {
    return fail(Errc::invalid_state,
                "plan " + std::to_string(id.value()) + " is not an applied user task");
  }
  Result<Undo> stored = fail(Errc::not_found, "not read");
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        stored = actions::load_undo(db, id);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  if (!stored->reversible()) {
    return fail(Errc::not_reversible, "not reversible: " + stored->not_reversible);
  }
  const std::scoped_lock lock(mutex_);
  undos_.insert(id.value());
  claims_.erase(id.value());
  return state;
}

}  // namespace myclean::cleaning
