#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health/analysis.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/process_table.hpp"
#include "core/telemetry/registration.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "core/telemetry/volume_info.hpp"
#include "core/trust/trust.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The OS sources the telemetry component samples; the Win32 ones in the service, fakes in tests.
struct TelemetrySources {
  std::unique_ptr<telemetry::IProcessTable> processes;
  std::unique_ptr<telemetry::IPerfCounters> counters;
  std::unique_ptr<telemetry::IMemoryStatus> memory;
  std::unique_ptr<telemetry::IVolumeInfo> volumes;
  std::string system_drive;  // the volume Windows runs from, as a series entity ("C:")
};

// The trust signal of the running process `pid` created at `created` (TrustComponent::process);
// fails when its image cannot be located or read.
using ProcessTrust = std::function<Result<trust::TrustSignal>(std::uint32_t pid, WallTime created)>;

// The newest successful sample of one collector.
struct Reading {
  WallTime time;
  std::vector<telemetry::Measurement> measurements;
};

// A collector that passes everything through to `inner` and keeps its newest measurements, so
// that metrics.latest reads them without scanning the store's series.
class Tap final : public telemetry::ICollector {
 public:
  explicit Tap(std::unique_ptr<telemetry::ICollector> inner) : inner_(std::move(inner)) {}

  [[nodiscard]] std::string_view name() const override { return inner_->name(); }
  [[nodiscard]] std::chrono::seconds interval() const override { return inner_->interval(); }
  [[nodiscard]] Result<std::vector<telemetry::Measurement>> collect(WallTime now,
                                                                    MonoTime monotonic) override;

  // Null before the first successful sample. Thread-safe.
  [[nodiscard]] std::shared_ptr<const Reading> latest() const { return latest_.get(); }

 private:
  std::unique_ptr<telemetry::ICollector> inner_;
  telemetry::Latest<Reading> latest_;
};

struct Collectors {
  telemetry::LiveViews live;
  std::vector<const Tap*> taps;    // owned by the pipeline
  std::vector<Error> unavailable;  // as telemetry::Registration::unavailable
};

// Adds the collectors of telemetry::register_collectors (same set, same intervals) to `pipeline`,
// each behind a Tap. `sources` must outlive the pipeline.
[[nodiscard]] Collectors add_collectors(telemetry::Pipeline& pipeline, TelemetrySources& sources);

// The telemetry read methods (contracts in core/ipc/telemetry.hpp). Thread-safe: it reads only
// the collectors' latest views, the store and the memory source.
class TelemetryMethods {
 public:
  // Everything must outlive this object. Without `trust`, every "trust" is null.
  TelemetryMethods(const Collectors& collectors, store::Store& store, const os::IClock& clock,
                   const TelemetrySources& sources, ProcessTrust trust = {});

  // Registers metrics.latest, metrics.query, processes.top, processes.explain and volumes.list.
  [[nodiscard]] Result<void> register_methods(ipc::Dispatcher& methods) const;

  [[nodiscard]] Result<nlohmann::json> latest() const;
  [[nodiscard]] Result<nlohmann::json> query(const nlohmann::json& params) const;
  [[nodiscard]] Result<nlohmann::json> top(const nlohmann::json& params) const;
  [[nodiscard]] Result<nlohmann::json> explain(const nlohmann::json& params) const;
  [[nodiscard]] Result<nlohmann::json> volumes() const;

  // The alert facts of every volume: volume.free_percent (the system volume only),
  // volume.free_gib and volume.days_until_full (infinite when not filling, absent while there is
  // too little history), entity the volume entity; each detail starts with the entity ("C:").
  [[nodiscard]] Result<std::vector<alerts::Reading>> volume_facts() const;

 private:
  [[nodiscard]] Result<nlohmann::json> volume_list() const;
  [[nodiscard]] std::optional<trust::TrustSignal> signal_of(
      const telemetry::ProcessInfo& process) const;
  // The trust JSON of a process, null when unknown.
  [[nodiscard]] nlohmann::json trust_of(const telemetry::ProcessInfo& process) const;

  const Collectors* collectors_;
  store::Store* store_;
  const os::IClock* clock_;
  const telemetry::IMemoryStatus* memory_;
  unsigned logical_processors_;
  std::string system_drive_;
  ProcessTrust trust_;
};

// Samples the machine on a thread of its own: a telemetry::Pipeline over every collector, with
// the read methods above. A collector this machine lacks is logged and reported as unavailable
// in `status`; one that fails while sampling makes the component degraded until it has not
// failed for failure_window. The first sample is taken in start(), so the methods have data from
// the first request on. After every new volume sample, the volume facts go to `alerts`.
class TelemetryComponent final : public IComponent {
 public:
  static constexpr std::chrono::seconds failure_window{120};  // twice the longest interval
  static constexpr std::chrono::seconds stall_limit{30};      // no poll for this long: degraded

  // `alerts` (may be null) must outlive the component.
  explicit TelemetryComponent(TelemetrySources sources, ProcessTrust trust = {},
                              health::IAlertSink* alerts = nullptr);
  TelemetryComponent(const TelemetryComponent&) = delete;
  TelemetryComponent& operator=(const TelemetryComponent&) = delete;
  TelemetryComponent(TelemetryComponent&&) = delete;
  TelemetryComponent& operator=(TelemetryComponent&&) = delete;
  ~TelemetryComponent() override;

  [[nodiscard]] std::string_view name() const override { return "telemetry"; }
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // The newest volume table; null before the first volume sample. Thread-safe, after start().
  [[nodiscard]] std::shared_ptr<const telemetry::VolumeView> volumes() const;
  // The newest sample of `metric` for `entity` ("disk.idle", "0"); nullopt when none was taken.
  // Thread-safe, after start().
  [[nodiscard]] std::optional<double> latest_value(std::string_view metric,
                                                   std::string_view entity) const;

 private:
  struct Failure {
    std::string message;
    MonoTime time;
  };

  void poll();
  void report_volumes();
  void run(const std::stop_token& stop);

  TelemetrySources sources_;
  ProcessTrust trust_;
  health::IAlertSink* alerts_;
  std::optional<WallTime> volumes_reported_;  // the sampling thread only
  const os::IClock* clock_ = nullptr;
  std::unique_ptr<telemetry::Pipeline> pipeline_;  // the sampling thread only, after start()
  Collectors collectors_;
  std::unique_ptr<TelemetryMethods> methods_;
  std::jthread thread_;

  mutable std::mutex mutex_;                              // guards the members below
  std::map<std::string, Failure, std::less<>> failures_;  // by collector name
  std::optional<MonoTime> last_poll_;
};

}  // namespace myclean::svc
