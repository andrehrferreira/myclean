#include "svc/resident_component.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/alerts/engine.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/resident.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"
#include "core/resident/leak.hpp"
#include "core/resident/tracker.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "svc/component.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;

}  // namespace

ResidentComponent::~ResidentComponent() {
  stop();
}

std::vector<store::MigrationSet> ResidentComponent::migrations() const {
  return {resident::resident_migrations()};
}

Result<void> ResidentComponent::start(const Context& context) {
  store_ = &context.store();
  clock_ = &context.clock();
  for (store::RetentionRule& rule : resident::resident_retention()) {
    if (Result<void> added = store_->add_retention(std::move(rule)); !added) {
      return added;
    }
  }
  {
    const std::scoped_lock lock(mutex_);
    tracker_ = std::make_unique<resident::Tracker>(*store_);
  }
  note(tracker_->load(clock_->now()));
  using ipc::Param;
  using ipc::ParamType;
  Result<void> registered =
      context.methods()
          .add(std::string(ipc::resident_costs_method), {}, [this](const Json&) { return costs(); })
          .and_then([&] {
            return context.methods().add(
                std::string(ipc::resident_trend_method),
                {Param{.name = "key", .type = ParamType::token, .required = true}},
                [this](const Json& params) { return trend(params); });
          });
  if (!registered) {
    return registered;
  }
  if (sources_.view && sources_.period > std::chrono::milliseconds::zero()) {
    // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  }
  return {};
}

void ResidentComponent::run(const std::stop_token& stop) {
  std::mutex wait_mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(wait_mutex);
  while (!stop.stop_requested()) {
    tick();
    static_cast<void>(wake.wait_for(lock, stop, sources_.period, [] { return false; }));
  }
}

void ResidentComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  const std::scoped_lock lock(mutex_);
  if (tracker_ && !flushed_) {
    tracker_->flush();
    flushed_ = true;
  }
}

void ResidentComponent::tick() {
  const std::shared_ptr<const telemetry::ProcessView> view =
      sources_.view ? sources_.view() : nullptr;
  if (!view) {
    return;
  }
  const resident::TrafficSource traffic =
      sources_.network ? resident::TrafficSource([this](WallTime from, WallTime to) {
        return sources_.network(*store_, from, to);
      })
                       : resident::TrafficSource{};
  Result<std::vector<alerts::Reading>> ticked = [&] {
    const std::scoped_lock lock(mutex_);
    return tracker_->tick(*view, traffic);
  }();
  if (!ticked) {
    note(tl::unexpected(std::move(ticked.error())));
    return;
  }
  if (alerts_ != nullptr && !ticked->empty()) {
    alerts_->report(*ticked);
  }
  note({});
}

void ResidentComponent::note(const Result<void>& outcome) {
  const std::scoped_lock lock(mutex_);
  if (outcome) {
    problem_.reset();
  } else if (problem_ != outcome.error().message) {
    problem_ = outcome.error().message;
    spdlog::warn("resident: {}", *problem_);
  }
}

ipc::ComponentHealth ResidentComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  const std::scoped_lock lock(mutex_);
  if (problem_) {
    health.problems.push_back(*problem_);
  }
  health.healthy = health.problems.empty();
  return health;
}

Result<Json> ResidentComponent::costs() const {
  const WallTime from = clock_->now() - std::chrono::days{ipc::resident_costs_days};
  Result<std::vector<resident::Cost>> list =
      resident::costs(*store_, from, std::chrono::hours{ipc::resident_min_hours});
  if (!list) {
    return tl::unexpected(list.error());
  }
  Json programs = Json::array();
  for (const resident::Cost& cost : *list) {
    programs.push_back(Json{{"image", cost.key},
                            {"key", ipc::entity_token(cost.key)},
                            {"hours_running", static_cast<double>(cost.running.count()) / 60.0},
                            {"cpu_seconds", cost.cpu_seconds},
                            {"ram_average", cost.ram_average.count()},
                            {"ram_peak", cost.ram_peak.count()},
                            {"disk_bytes", cost.disk_bytes},
                            {"net_bytes", cost.net_bytes}});
  }
  return Json{{"days", ipc::resident_costs_days},
              {"from", store::to_unix_seconds(from)},
              {"programs", std::move(programs)}};
}

Result<Json> ResidentComponent::trend(const Json& params) const {
  const std::string key = params.at("key").get<std::string>();
  const std::scoped_lock lock(mutex_);
  for (const auto& [image, points] : tracker_->history().trends()) {
    if (ipc::entity_token(image) != key) {
      continue;
    }
    Json list = Json::array();
    for (const resident::Point& point : points) {
      list.push_back(Json{{"time", store::to_unix_seconds(point.time)},
                          {"private_bytes", point.private_bytes.count()}});
    }
    Json leak = nullptr;
    if (const std::optional<resident::Growth> growth =
            resident::steady_growth(points, clock_->now())) {
      Json hourly = Json::array();
      for (const Bytes median : growth->hourly) {
        hourly.push_back(median.count());
      }
      leak = Json{{"hours", growth->hours}, {"hourly", std::move(hourly)}};
    }
    return Json{
        {"image", image}, {"key", key}, {"points", std::move(list)}, {"leak", std::move(leak)}};
  }
  return fail(Errc::not_found, "no program " + key + " in the last 48 hours");
}

}  // namespace myclean::svc
