#include "svc/startup_component.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/events/event_store.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/impact.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/process_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {
namespace {

std::vector<startup::Usage> usage_of(const telemetry::ProcessView* view) {
  std::vector<startup::Usage> usage;
  if (view == nullptr) {
    return usage;
  }
  usage.reserve(view->rows.size());
  for (const telemetry::ProcessRow& row : view->rows) {
    usage.push_back(startup::Usage{.pid = row.info.pid,
                                   .created = row.info.created,
                                   .image = row.info.image,
                                   .cpu = row.info.cpu_time,
                                   .io_bytes = row.info.io_bytes});
  }
  return usage;
}

}  // namespace

StartupComponent::~StartupComponent() {
  stop();
}

std::vector<store::MigrationSet> StartupComponent::migrations() const {
  return {startup::impact_migrations()};
}

Result<void> StartupComponent::start(const Context& context) {
  if (!sources_.view || !sources_.logon || context.owner_sid().empty()) {
    return {};  // nothing to measure: no process table or no owner
  }
  // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
  worker_ = std::jthread([this, context](const std::stop_token& stop) { run(stop, context); });
  return {};
}

void StartupComponent::run(const std::stop_token& stop, const Context& context) {
  startup::LogonRecorder recorder(context.store());
  std::mutex wait_mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(wait_mutex);
  while (!stop.stop_requested()) {
    tick(context, recorder);
    static_cast<void>(wake.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; }));
  }
}

void StartupComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

void StartupComponent::tick(const Context& context, startup::LogonRecorder& recorder) {
  const Result<WallTime> logon = sources_.logon(context.owner_sid());
  const std::shared_ptr<const telemetry::ProcessView> view = sources_.view();
  const startup::StartSource starts =
      [&context](WallTime from, WallTime to) -> Result<std::vector<startup::Start>> {
    Result<std::vector<events::StoredStart>> stored =
        events::process_starts(context.store(), from, to);
    if (!stored) {
      return tl::unexpected(stored.error());
    }
    std::vector<startup::Start> starts;
    starts.reserve(stored->size());
    for (events::StoredStart& start : *stored) {
      starts.push_back(startup::Start{.pid = start.pid,
                                      .time = start.time,
                                      .parent_pid = start.parent_pid,
                                      .parent_time = start.parent_time,
                                      .image = std::move(start.image)});
    }
    return starts;
  };
  const Result<void> ticked =
      recorder.tick(context.clock().now(), logon ? std::optional{*logon} : std::nullopt,
                    usage_of(view.get()), starts);
  const std::scoped_lock lock(mutex_);
  if (ticked) {
    problem_.reset();
  } else if (problem_ != ticked.error().message) {
    problem_ = ticked.error().message;
    spdlog::warn("startup: {}", *problem_);
  }
}

ipc::ComponentHealth StartupComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  const std::scoped_lock lock(mutex_);
  if (problem_) {
    health.problems.push_back(*problem_);
  }
  health.healthy = health.problems.empty();
  return health;
}

}  // namespace myclean::svc
