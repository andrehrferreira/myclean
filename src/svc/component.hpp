#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::svc {

// What the service hands a component when it starts. Everything outlives the component's
// stop().
class Context {
 public:
  Context(store::Store& store, ipc::Dispatcher& methods, const os::IClock& clock,
          std::filesystem::path data_dir)
      : store_(&store), methods_(&methods), clock_(&clock), data_dir_(std::move(data_dir)) {}

  // Telemetry goes in with Store::record() and Store::enqueue(); the service flushes them.
  [[nodiscard]] store::Store& store() const { return *store_; }
  // Register IPC methods here, during start(); the pipe opens after every component started.
  [[nodiscard]] ipc::Dispatcher& methods() const { return *methods_; }
  [[nodiscard]] const os::IClock& clock() const { return *clock_; }
  // The build's read-only data folder (<version>\data): rules, classes, knowledge base.
  [[nodiscard]] const std::filesystem::path& data_dir() const { return data_dir_; }

 private:
  store::Store* store_;
  ipc::Dispatcher* methods_;
  const os::IClock* clock_;
  std::filesystem::path data_dir_;
};

// A long-running part of the service, such as a collector. The service starts its components in
// registration order after the store is open and stops them in reverse order before the store
// is flushed and closed.
class IComponent {
 public:
  virtual ~IComponent() = default;
  IComponent(const IComponent&) = delete;
  IComponent& operator=(const IComponent&) = delete;
  IComponent(IComponent&&) = delete;
  IComponent& operator=(IComponent&&) = delete;

  // A short unique name for logs and `status`.
  [[nodiscard]] virtual std::string_view name() const = 0;
  // The schema of the tables this component owns, applied when the store opens.
  [[nodiscard]] virtual std::vector<store::MigrationSet> migrations() const { return {}; }
  // Starts the component's own threads or timers. A failure stops the service start (contract
  // C7): components started before are stopped and the store is closed.
  [[nodiscard]] virtual Result<void> start(const Context& context) = 0;
  // Stops all work and returns; no store or context access afterwards. The whole service stop
  // must fit in 30 s (contract C3), so this must not block for long.
  virtual void stop() = 0;
  // The component's health for `status`, between start() and stop(). Called from IPC threads,
  // so it must be thread-safe. Healthy with no remarks unless overridden.
  [[nodiscard]] virtual ipc::ComponentHealth health() const {
    return ipc::ComponentHealth{.name = std::string(name())};
  }

 protected:
  IComponent() = default;
};

}  // namespace myclean::svc
