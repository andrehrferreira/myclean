#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/source.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The newest volume table of the telemetry component; null before its first sample.
using VolumeViews = std::function<std::shared_ptr<const telemetry::VolumeView>()>;

// Disk health (disk-health spec) as a service component: a health::HealthCollector on a
// telemetry::Pipeline and thread of its own, because one read can wait up to 60 s on WMI and
// must never delay the 1 s collectors. Its alert facts go to `alerts`. Registers disks.health
// (core/ipc/disk_health.hpp).
//
// An invalid data/drive-specs.json leaves every model without rated limits; a failed read keeps
// the previous view. Both are reported as problems in `status` (a read failure until a later
// read succeeds).
class DiskHealthComponent final : public IComponent {
 public:
  // `files`, `alerts` and what `volumes` reads must outlive the component.
  DiskHealthComponent(std::unique_ptr<health::IDiskHealthSource> source,
                      const rules::IDataFiles& files, health::IAlertSink& alerts,
                      VolumeViews volumes);
  DiskHealthComponent(const DiskHealthComponent&) = delete;
  DiskHealthComponent& operator=(const DiskHealthComponent&) = delete;
  DiskHealthComponent(DiskHealthComponent&&) = delete;
  DiskHealthComponent& operator=(DiskHealthComponent&&) = delete;
  ~DiskHealthComponent() override;

  [[nodiscard]] std::string_view name() const override { return "disk_health"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  // Waits for a read in progress to finish.
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // disks.health. Thread-safe.
  [[nodiscard]] Result<nlohmann::json> drives() const;
  // The newest reading of every drive; null before start() and before the first read.
  // Thread-safe.
  [[nodiscard]] std::shared_ptr<const health::HealthView> latest() const {
    return collector_ != nullptr ? collector_->latest() : nullptr;
  }

 private:
  void run(const std::stop_token& stop);

  std::unique_ptr<health::IDiskHealthSource> source_;
  const rules::IDataFiles* files_;
  health::IAlertSink* alerts_;
  VolumeViews volumes_;
  const os::IClock* clock_ = nullptr;
  std::optional<std::string> specs_problem_;       // set in start() only
  health::HealthCollector* collector_ = nullptr;   // owned by pipeline_
  std::unique_ptr<telemetry::Pipeline> pipeline_;  // the component's thread only, after start()
  std::jthread thread_;

  mutable std::mutex mutex_;  // guards read_problem_
  std::optional<std::string> read_problem_;
};

}  // namespace myclean::svc
