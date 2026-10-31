#include "svc/alerts_component.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/alerts/config.hpp"
#include "core/alerts/engine.hpp"
#include "core/alerts/schema.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;

Json optional_number(const std::optional<double>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json optional_time(const std::optional<WallTime>& time) {
  return time ? Json(store::to_unix_seconds(*time)) : Json(nullptr);
}

}  // namespace

std::vector<store::MigrationSet> AlertsComponent::migrations() const {
  return {alerts::alert_migrations()};
}

Result<void> AlertsComponent::start(const Context& context) {
  clock_ = &context.clock();
  Result<alerts::AlertConfig> config = alerts::load_alert_config(*files_, context.data_dir());
  if (!config) {
    config_problem_ = "alert rules not loaded, no alert can fire: " + config.error().message;
    spdlog::error("alerts: {}", *config_problem_);
  }
  Result<std::unique_ptr<alerts::AlertEngine>> engine = alerts::AlertEngine::open(
      context.store(), context.clock(), config ? std::move(*config) : alerts::AlertConfig{});
  if (engine) {
    engine_ = std::move(*engine);
  } else {
    engine_problem_ = "alert engine unavailable: " + engine.error().message;
    spdlog::error("alerts: {}", *engine_problem_);
  }

  using ipc::Param;
  using ipc::ParamType;
  ipc::Dispatcher& methods = context.methods();
  return methods
      .add(std::string(ipc::alerts_list_method),
           {Param{.name = "since", .type = ParamType::integer}},
           [this](const Json& params) { return list(params); })
      .and_then([&] {
        return methods.add(std::string(ipc::alerts_badge_method), {},
                           [this](const Json&) { return badge(); });
      })
      .and_then([&] {
        return methods.add(std::string(ipc::alerts_snooze_method),
                           {Param{.name = "rule", .type = ParamType::token, .required = true},
                            Param{.name = "entity", .type = ParamType::token, .required = true},
                            Param{.name = "minutes", .type = ParamType::integer, .required = true}},
                           [this](const Json& params) { return snooze(params); });
      })
      .and_then([&] {
        return methods.add(std::string(ipc::alerts_dismiss_method),
                           {Param{.name = "rule", .type = ParamType::token, .required = true},
                            Param{.name = "entity", .type = ParamType::token, .required = true}},
                           [this](const Json& params) { return dismiss(params); });
      });
}

ipc::ComponentHealth AlertsComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  for (const std::optional<std::string>& problem : {config_problem_, engine_problem_}) {
    if (problem) {
      health.problems.push_back(*problem);
    }
  }
  const std::scoped_lock lock(mutex_);
  if (report_problem_) {
    health.problems.push_back(*report_problem_);
  }
  health.healthy = health.problems.empty();
  return health;
}

void AlertsComponent::report(std::span<const alerts::Reading> readings) {
  if (!engine_ || readings.empty()) {
    return;
  }
  Result<std::vector<alerts::Alert>> toasts = engine_->report(readings);
  const WallTime now = clock_->now();
  const std::scoped_lock lock(mutex_);
  if (!toasts) {
    if (report_problem_ != toasts.error().message) {
      spdlog::error("alerts: {}", toasts.error().message);
    }
    report_problem_ = toasts.error().message;
    return;
  }
  report_problem_.reset();
  for (const alerts::Alert& alert : *toasts) {
    spdlog::info("alert: {} for {}: {}", alert.key.rule, alert.key.entity, alert.title);
    toasted_.insert_or_assign(alert.key, now);
  }
}

Result<alerts::AlertEngine*> AlertsComponent::engine() const {
  if (!engine_) {
    return fail(Errc::invalid_state, engine_problem_.value_or("alert engine unavailable"));
  }
  return engine_.get();
}

Result<Json> AlertsComponent::list(const Json& params) const {
  const std::int64_t since = params.value("since", std::int64_t{0});
  if (since < 0) {
    return fail(Errc::invalid_argument, "alerts.list: since must be Unix seconds");
  }
  Result<alerts::AlertEngine*> engine = this->engine();
  if (!engine) {
    return tl::unexpected(engine.error());
  }
  Result<std::vector<alerts::Alert>> alerts =
      (*engine)->list(WallTime{std::chrono::seconds{since}});
  if (!alerts) {
    return tl::unexpected(alerts.error());
  }
  Json list = Json::array();
  const std::scoped_lock lock(mutex_);
  for (const alerts::Alert& alert : *alerts) {
    const auto toasted = toasted_.find(alert.key);
    list.push_back(Json{{"rule", alert.key.rule},
                        {"entity", ipc::entity_token(alert.key.entity)},
                        {"title", alert.title},
                        {"severity", alerts::to_string(alert.severity)},
                        {"first_at", store::to_unix_seconds(alert.first)},
                        {"last_at", store::to_unix_seconds(alert.last)},
                        {"count", alert.count},
                        {"active", alert.active},
                        {"snoozed_until", optional_time(alert.snoozed_until)},
                        {"toast_at", optional_time(toasted == toasted_.end()
                                                       ? std::nullopt
                                                       : std::optional<WallTime>{toasted->second})},
                        {"detail", alert.detail},
                        {"value", alert.value},
                        {"limit", optional_number(alert.limit)},
                        {"previous", optional_number(alert.previous)},
                        {"unit", alert.unit}});
  }
  return Json{{"alerts", std::move(list)}};
}

Result<Json> AlertsComponent::badge() const {
  return engine().transform([](const alerts::AlertEngine* engine) {
    const alerts::Badge badge = engine->badge();
    return Json{{"count", badge.count},
                {"worst", badge.worst ? alerts::to_string(*badge.worst) : ipc::badge_none}};
  });
}

Result<alerts::AlertKey> AlertsComponent::resolve(const Json& params) const {
  const auto& rule = params.at("rule").get_ref<const std::string&>();
  const auto& entity = params.at("entity").get_ref<const std::string&>();
  Result<alerts::AlertEngine*> engine = this->engine();
  if (!engine) {
    return tl::unexpected(engine.error());
  }
  Result<std::vector<alerts::Alert>> stored = (*engine)->list(WallTime{});
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  for (const alerts::Alert& alert : *stored) {
    if (alert.key.rule == rule && ipc::entity_token(alert.key.entity) == entity) {
      return alert.key;
    }
  }
  return fail(Errc::not_found, "no alert for rule '" + rule + "' and entity '" + entity + "'");
}

Result<Json> AlertsComponent::snooze(const Json& params) {
  const auto minutes = params.at("minutes").get<std::int64_t>();
  if (minutes < 1 || minutes > ipc::max_snooze_minutes) {
    return fail(Errc::invalid_argument,
                "alerts.snooze: minutes must be 1 to " + std::to_string(ipc::max_snooze_minutes));
  }
  Result<alerts::AlertKey> key = resolve(params);
  if (!key) {
    return tl::unexpected(key.error());
  }
  // The engine snoozes from its own now(), the current second.
  const WallTime until =
      std::chrono::floor<std::chrono::seconds>(clock_->now()) + std::chrono::minutes{minutes};
  return engine_->snooze(*key, std::chrono::minutes{minutes}).transform([until] {
    return Json{{"snoozed_until", store::to_unix_seconds(until)}};
  });
}

Result<Json> AlertsComponent::dismiss(const Json& params) {
  return resolve(params)
      .and_then([this](const alerts::AlertKey& key) { return engine_->dismiss(key); })
      .transform([] { return Json::object(); });
}

}  // namespace myclean::svc
