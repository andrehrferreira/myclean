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

#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/impact.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/process_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

struct StartupSources {
  // The telemetry component's newest process table (no query of its own).
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  // When the current logon of the owner with this SID began (owner_logon_time).
  std::function<Result<WallTime>(std::string_view owner_sid)> logon;
  // Windows' Diagnostics-Performance boot events; null: no boot is recorded.
  std::unique_ptr<startup::IDiagnosticsLog> diagnostics;
};

// Measures what each program uses in the first 2 minutes after each logon of the owner
// (startup::LogonRecorder over the telemetry samples and the events component's process starts),
// once a second on a thread of its own, and records each boot Windows measured (table boot_time)
// from the Diagnostics-Performance log every boot_interval. Owns tables logon_impact and
// boot_time, which startup.list and startup.boot_times read.
class StartupComponent final : public IComponent {
 public:
  explicit StartupComponent(StartupSources sources) : sources_(std::move(sources)) {}
  StartupComponent(const StartupComponent&) = delete;
  StartupComponent& operator=(const StartupComponent&) = delete;
  StartupComponent(StartupComponent&&) = delete;
  StartupComponent& operator=(StartupComponent&&) = delete;
  ~StartupComponent() override;

  // How often the Diagnostics-Performance log is read for new boots (Windows writes event 100
  // a few minutes after each boot).
  static constexpr std::chrono::minutes boot_interval{10};

  [[nodiscard]] std::string_view name() const override { return "startup"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

 private:
  void run(const std::stop_token& stop, const Context& context);
  void tick(const Context& context, startup::LogonRecorder& recorder);
  void record_boots(const Context& context);
  // Whether logons can be measured: a process table, the logon source and an owner.
  [[nodiscard]] bool measures(const Context& context) const;

  StartupSources sources_;
  std::jthread worker_;
  mutable std::mutex mutex_;  // guards problem_ and boot_problem_
  std::optional<std::string> problem_;
  std::optional<std::string> boot_problem_;
};

}  // namespace myclean::svc
