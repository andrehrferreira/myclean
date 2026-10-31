#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/alerts/engine.hpp"
#include "core/health/analysis.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/migrations.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The alert engine as a service component and the sink every producer reports its facts to
// (volume facts from telemetry, health facts from disk health). Registers alerts.list,
// alerts.badge, alerts.snooze and alerts.dismiss (core/ipc/alerts.hpp).
//
// Failures stay inside: an invalid data/alerts.toml leaves the engine without rules, and an
// engine that cannot open makes report() a no-op and the methods fail with invalid_state; both
// are reported as problems in `status`.
class AlertsComponent final : public IComponent, public health::IAlertSink {
 public:
  // `files` must outlive the component.
  explicit AlertsComponent(const rules::IDataFiles& files) : files_(&files) {}

  [[nodiscard]] std::string_view name() const override { return "alerts"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override {}
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // Evaluates the facts and remembers which alerts the engine decided to toast. Thread-safe.
  void report(std::span<const alerts::Reading> readings) override;

  [[nodiscard]] Result<nlohmann::json> list(const nlohmann::json& params) const;
  [[nodiscard]] Result<nlohmann::json> badge() const;
  [[nodiscard]] Result<nlohmann::json> snooze(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> dismiss(const nlohmann::json& params);

 private:
  [[nodiscard]] Result<alerts::AlertEngine*> engine() const;
  // The stored alert key whose rule is `rule` and whose entity has the token form `entity`.
  [[nodiscard]] Result<alerts::AlertKey> resolve(const nlohmann::json& params) const;

  const rules::IDataFiles* files_;
  const os::IClock* clock_ = nullptr;
  std::unique_ptr<alerts::AlertEngine> engine_;
  std::optional<std::string> config_problem_;  // set in start() only
  std::optional<std::string> engine_problem_;  // set in start() only

  mutable std::mutex mutex_;  // guards the members below
  std::map<alerts::AlertKey, WallTime> toasted_;
  std::optional<std::string> report_problem_;  // the last report() failed
};

}  // namespace myclean::svc
