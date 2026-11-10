#include "svc/elimination_component.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/elimination/keeper.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/elimination/views.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/process_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;

Result<void> not_started() {
  return fail(Errc::invalid_state, "guided freeze elimination is not running");
}

}  // namespace

EliminationComponent::~EliminationComponent() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

std::vector<store::MigrationSet> EliminationComponent::migrations() const {
  return {elimination::elimination_migrations()};
}

Result<void> EliminationComponent::start(const Context& context) {
  store_ = &context.store();
  clock_ = &context.clock();
  keeper_ = std::make_unique<elimination::PauseKeeper>(
      *store_, *clock_,
      elimination::KeeperSources{.processes = sources_.processes.get(),
                                 .view = sources_.view,
                                 .protects = sources_.protects,
                                 .alerts = sources_.alerts});
  keeper_->recover();
  if (const std::string problem = keeper_->problem(); !problem.empty()) {
    spdlog::warn("elimination: {}", problem);
  }
  Result<void> added =
      context.methods()
          .add(std::string(ipc::input_pauses_method), {},
               [this](const Json& /*params*/) { return pauses_answer(); })
          .and_then([&] {
            return context.methods().add(
                std::string(ipc::input_end_pause_method), {},
                [this](const Json& /*params*/) { return end_active(); }, ipc::Access::tray);
          });
  if (!added) {
    return added;
  }
  worker_ = std::jthread([this](const std::stop_token& stop) {
    std::mutex wait_mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(wait_mutex);
    while (!stop.stop_requested()) {
      tick();
      static_cast<void>(wake.wait_for(lock, stop, tick_interval, [] { return false; }));
    }
  });
  return {};
}

void EliminationComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  if (keeper_ != nullptr) {
    keeper_->stop();
  }
}

void EliminationComponent::tick() {
  if (keeper_ != nullptr) {
    keeper_->tick();
  }
}

ipc::ComponentHealth EliminationComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  if (keeper_ != nullptr) {
    if (std::string problem = keeper_->problem(); !problem.empty()) {
      health.problems.push_back(std::move(problem));
    }
  }
  return health;
}

Result<elimination::PauseRecord> EliminationComponent::pause(PlanId plan, std::string_view image,
                                                             elimination::PauseMode mode,
                                                             std::chrono::hours period) {
  if (keeper_ == nullptr) {
    return tl::unexpected(not_started().error());
  }
  return keeper_->pause(plan, image, mode, period);
}

Result<void> EliminationComponent::end(PlanId plan, elimination::EndReason reason) {
  if (keeper_ == nullptr) {
    return not_started();
  }
  return keeper_->end(plan, reason);
}

Result<std::optional<elimination::PauseRecord>> EliminationComponent::record(PlanId plan) const {
  if (keeper_ == nullptr) {
    return tl::unexpected(not_started().error());
  }
  return keeper_->record(plan);
}

Result<std::optional<elimination::PauseRecord>> EliminationComponent::active() const {
  if (keeper_ == nullptr) {
    return tl::unexpected(not_started().error());
  }
  return keeper_->active();
}

Result<Json> EliminationComponent::pauses_answer() const {
  return elimination::pauses_answer(*store_, sources_.view ? sources_.view() : nullptr,
                                    clock_->now());
}

Result<Json> EliminationComponent::end_active() {
  Result<std::optional<elimination::PauseRecord>> pause = keeper_->active();
  if (!pause) {
    return tl::unexpected(pause.error());
  }
  const std::optional<elimination::PauseRecord>& active = *pause;
  if (!active.has_value()) {
    return fail(Errc::not_found, "no pause is active");
  }
  if (Result<void> ended = keeper_->end(active->plan, elimination::EndReason::ended_by_owner);
      !ended) {
    return tl::unexpected(ended.error());
  }
  return Json{{"plan", active->plan.value()}, {"ended", true}};
}

}  // namespace myclean::svc
