#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/elimination/keeper.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/health/analysis.hpp"
#include "core/ipc/status.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/os/process_control.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The OS sources of guided freeze elimination; Win32 and the governor in the service, fakes in
// tests.
struct EliminationSources {
  std::unique_ptr<os::IProcessControl> processes;  // null: no suspend pause
  // The telemetry component's newest process table (no query of its own).
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  // Whether a process must never be suspended (governing::Governor::protects); empty: every
  // process is protected.
  std::function<bool(const os::ProcessKey&)> protects;
  health::IAlertSink* alerts = nullptr;  // the reminder after a guided pause (must outlive it)
};

// Guided freeze elimination in the service (input-watchdog spec I3): the pauses of
// elimination::PauseKeeper, ticked every tick_interval on a thread of its own, recovered at the
// start (a suspend pause a crash left behind is resumed as recorded) and ended at the stop (a
// suspend pause is resumed; a guided one goes on). It is the elimination::IPauses the cleaning
// executor runs approved PauseSuspect plans with. Registers input.pauses and the tray-only
// input.end_pause (core/ipc/input.hpp). Owns tables suspect_pause, suspect_pause_process and
// suspect_pause_span.
class EliminationComponent final : public IComponent, public elimination::IPauses {
 public:
  static constexpr std::chrono::seconds tick_interval{5};

  explicit EliminationComponent(EliminationSources sources) : sources_(std::move(sources)) {}
  EliminationComponent(const EliminationComponent&) = delete;
  EliminationComponent& operator=(const EliminationComponent&) = delete;
  EliminationComponent(EliminationComponent&&) = delete;
  EliminationComponent& operator=(EliminationComponent&&) = delete;
  ~EliminationComponent() override;

  [[nodiscard]] std::string_view name() const override { return "elimination"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // elimination::IPauses, failing with invalid_state outside start() .. stop().
  [[nodiscard]] Result<elimination::PauseRecord> pause(PlanId plan, std::string_view image,
                                                       elimination::PauseMode mode,
                                                       std::chrono::hours period) override;
  [[nodiscard]] Result<void> end(PlanId plan, elimination::EndReason reason) override;
  [[nodiscard]] Result<std::optional<elimination::PauseRecord>> record(PlanId plan) const override;
  [[nodiscard]] Result<std::optional<elimination::PauseRecord>> active() const override;

  // One tick of the keeper. Public for tests, which drive time with a fake clock and no thread.
  void tick();

 private:
  [[nodiscard]] Result<nlohmann::json> pauses_answer() const;
  [[nodiscard]] Result<nlohmann::json> end_active();

  EliminationSources sources_;
  store::Store* store_ = nullptr;      // set in start()
  const os::IClock* clock_ = nullptr;  // set in start()
  std::unique_ptr<elimination::PauseKeeper> keeper_;
  std::jthread worker_;
};

}  // namespace myclean::svc
