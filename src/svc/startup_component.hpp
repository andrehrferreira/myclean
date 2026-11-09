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

#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
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
};

// Measures what each program uses in the first 2 minutes after each logon of the owner
// (startup::LogonRecorder over the telemetry samples and the events component's process starts),
// once a second on a thread of its own. Owns table logon_impact, which startup.list reads.
class StartupComponent final : public IComponent {
 public:
  explicit StartupComponent(StartupSources sources) : sources_(std::move(sources)) {}
  StartupComponent(const StartupComponent&) = delete;
  StartupComponent& operator=(const StartupComponent&) = delete;
  StartupComponent(StartupComponent&&) = delete;
  StartupComponent& operator=(StartupComponent&&) = delete;
  ~StartupComponent() override;

  [[nodiscard]] std::string_view name() const override { return "startup"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

 private:
  void run(const std::stop_token& stop, const Context& context);
  void tick(const Context& context, startup::LogonRecorder& recorder);

  StartupSources sources_;
  std::jthread worker_;
  mutable std::mutex mutex_;  // guards problem_
  std::optional<std::string> problem_;
};

}  // namespace myclean::svc
