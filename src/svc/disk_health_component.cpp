#include "svc/disk_health_component.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/history.hpp"
#include "core/health/reading.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;

template <typename T>
Json optional_value(const std::optional<T>& value) {
  return value ? Json(*value) : Json(nullptr);
}

std::string_view status_text(health::Status status) {
  switch (status) {
    case health::Status::good:
      return ipc::drive_good;
    case health::Status::warning:
      return ipc::drive_warning;
    case health::Status::critical:
      return ipc::drive_failing;
    case health::Status::unknown:
      return ipc::drive_unknown;
  }
  return ipc::drive_unknown;
}

// The drive's own health data was read, not only Windows' reliability counters.
bool readable(health::Source source) {
  return source == health::Source::nvme_health_log || source == health::Source::ata_smart ||
         source == health::Source::sat_pass_through;
}

Json endurance_json(const health::Endurance& endurance) {
  std::string_view state = "projected";
  if (!endurance.written_tb) {
    state = "not_reported";
  } else if (!endurance.rated_tbw) {
    state = "rating_unknown";
  } else if (!endurance.years_left) {
    state = "insufficient_history";
  }
  return Json{{"state", state},
              {"years_left", optional_value(endurance.years_left)},
              {"tbw_rated", optional_value(endurance.rated_tbw)},
              {"tbw_used_percent", optional_value(endurance.used_percent)},
              {"note", endurance.unavailable}};
}

Json drive_json(const health::DriveHealth& drive, const telemetry::VolumeView* volumes) {
  const health::HealthReading& reading = drive.reading;
  Json mounts = Json::array();
  for (const telemetry::VolumeStatus& volume :
       volumes != nullptr ? volumes->volumes : std::vector<telemetry::VolumeStatus>{}) {
    if (std::ranges::find(volume.disks, reading.disk_number) != volume.disks.end()) {
      mounts.push_back(telemetry::volume_entity(volume));
    }
  }
  const bool read = readable(reading.source);
  return Json{
      {"key", ipc::entity_token(reading.drive)},
      {"disk", std::to_string(reading.disk_number)},
      {"model", reading.model},
      {"bus", health::to_string(reading.bus)},
      {"source", health::to_string(reading.source)},
      {"volumes", std::move(mounts)},
      {"status", status_text(drive.assessment.status)},
      {"reasons", drive.assessment.reasons},
      {"readable", read},
      {"unreadable_reason",
       read ? Json(nullptr)
            : Json(reading.problem.empty() ? "no health data could be read" : reading.problem)},
      {"temperature_c", optional_value(reading.temperature_c)},
      {"temperature_limit_c", optional_value(drive.assessment.temperature_limit_c)},
      {"percent_used", optional_value(reading.percentage_used)},
      {"available_spare", optional_value(reading.available_spare)},
      {"media_errors", optional_value(reading.media_errors)},
      {"reallocated_sectors", optional_value(reading.reallocated_sectors)},
      {"pending_sectors", optional_value(reading.pending_sectors)},
      {"power_on_hours", optional_value(reading.power_on_hours)},
      {"data_written_bytes", optional_value(reading.bytes_written)},
      {"endurance", endurance_json(drive.endurance)}};
}

}  // namespace

DiskHealthComponent::DiskHealthComponent(std::unique_ptr<health::IDiskHealthSource> source,
                                         const rules::IDataFiles& files, health::IAlertSink& alerts,
                                         VolumeViews volumes)
    : source_(std::move(source)), files_(&files), alerts_(&alerts), volumes_(std::move(volumes)) {}

DiskHealthComponent::~DiskHealthComponent() {
  stop();
}

std::vector<store::MigrationSet> DiskHealthComponent::migrations() const {
  return {health::health_migrations()};
}

Result<void> DiskHealthComponent::start(const Context& context) {
  clock_ = &context.clock();
  Result<health::DriveSpecs> specs = health::DriveSpecs::load(*files_, context.data_dir());
  if (!specs) {
    specs_problem_ = "drive specs not loaded, no rated limits are known: " + specs.error().message;
    spdlog::error("disk health: {}", *specs_problem_);
  }
  Result<std::unique_ptr<health::HealthCollector>> collector = health::HealthCollector::create(
      *source_, context.store(), specs ? std::move(*specs) : health::DriveSpecs{}, *alerts_,
      context.clock());
  if (!collector) {
    return tl::unexpected(collector.error());
  }
  collector_ = collector->get();
  pipeline_ = std::make_unique<telemetry::Pipeline>(context.clock(), context.store());
  pipeline_->add(std::move(*collector));
  if (Result<void> added = context.methods().add(std::string(ipc::disks_health_method), {},
                                                 [this](const Json&) { return drives(); });
      !added) {
    return added;
  }
  // The first read runs on the thread: it may take a minute.
  thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  return {};
}

void DiskHealthComponent::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void DiskHealthComponent::run(const std::stop_token& stop) {
  // Never sleep longer than this, so a test clock that does not move still polls.
  constexpr MonoTime::duration max_wait = std::chrono::seconds{1};
  std::mutex wait_mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(wait_mutex);
  while (!stop.stop_requested()) {
    const std::optional<MonoTime> due = pipeline_->next_due();
    if (!due || *due <= clock_->monotonic()) {
      const std::vector<Error> errors = pipeline_->poll();
      const std::scoped_lock guard(mutex_);
      if (errors.empty()) {
        read_problem_.reset();
      } else if (read_problem_ != errors.front().message) {
        read_problem_ = errors.front().message;
        spdlog::warn("disk health: {}", *read_problem_);
      }
      continue;
    }
    const MonoTime::duration wait =
        std::clamp(*due - clock_->monotonic(), MonoTime::duration::zero(), max_wait);
    static_cast<void>(wake.wait_for(lock, stop, wait, [] { return false; }));
  }
}

ipc::ComponentHealth DiskHealthComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  if (specs_problem_) {
    health.problems.push_back(*specs_problem_);
  }
  const std::scoped_lock lock(mutex_);
  if (read_problem_) {
    health.problems.push_back(*read_problem_);
  }
  health.healthy = health.problems.empty();
  return health;
}

Result<Json> DiskHealthComponent::drives() const {
  const std::shared_ptr<const health::HealthView> view = collector_->latest();
  const std::shared_ptr<const telemetry::VolumeView> volumes = volumes_();
  Json list = Json::array();
  if (view) {
    for (const health::DriveHealth& drive : view->drives) {
      list.push_back(drive_json(drive, volumes.get()));
    }
  }
  return Json{{"time", view ? Json(store::to_unix_seconds(view->time)) : Json(nullptr)},
              {"drives", std::move(list)}};
}

}  // namespace myclean::svc
