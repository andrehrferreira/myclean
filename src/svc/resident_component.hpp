#pragma once

#include <chrono>
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
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/sampler.hpp"
#include "core/resident/tracker.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

struct ResidentSources {
  // The telemetry component's newest process table (no query of its own).
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  // The network bytes per process of the stored minutes in [from, to) (the events component's
  // net_minute); null: no network bytes are recorded.
  std::function<Result<std::vector<resident::ProcessTraffic>>(store::Store& store, WallTime from,
                                                              WallTime to)>
      network;
  // How often the thread hands the newest table to the tracker; zero: no thread (tests call
  // tick() themselves).
  std::chrono::milliseconds period{1000};
};

// The resident-process history (core/resident) on a thread of its own: every second it hands
// the newest process table to a resident::Tracker, and reports the leak readings of each closed
// 5-minute bucket to `alerts` (rule process.memory-leak). It only reads: it has no way to
// suspend, cap or end a process. Owns tables resident_fine and resident_hour; registers
// resident.costs and resident.trend (core/ipc/resident.hpp). A failed read is reported as a
// problem in `status` until a later one succeeds.
class ResidentComponent final : public IComponent {
 public:
  // `alerts` (may be null) must outlive the component.
  explicit ResidentComponent(ResidentSources sources, health::IAlertSink* alerts = nullptr)
      : sources_(std::move(sources)), alerts_(alerts) {}
  ResidentComponent(const ResidentComponent&) = delete;
  ResidentComponent& operator=(const ResidentComponent&) = delete;
  ResidentComponent(ResidentComponent&&) = delete;
  ResidentComponent& operator=(ResidentComponent&&) = delete;
  ~ResidentComponent() override;

  [[nodiscard]] std::string_view name() const override { return "resident"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  // Records the minute observed so far.
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // resident.costs and resident.trend. Thread-safe.
  [[nodiscard]] Result<nlohmann::json> costs() const;
  [[nodiscard]] Result<nlohmann::json> trend(const nlohmann::json& params) const;

  // One pass of the thread: the newest process table to the tracker, the leak readings to
  // `alerts`. Public for tests, which drive time with a fake clock and no thread.
  void tick();

 private:
  void run(const std::stop_token& stop);
  void note(const Result<void>& outcome);

  ResidentSources sources_;
  health::IAlertSink* alerts_;
  store::Store* store_ = nullptr;
  const os::IClock* clock_ = nullptr;
  std::jthread worker_;

  mutable std::mutex mutex_;  // guards the members below
  std::unique_ptr<resident::Tracker> tracker_;
  bool flushed_ = false;  // stop() recorded the last minute
  std::optional<std::string> problem_;
};

}  // namespace myclean::svc
