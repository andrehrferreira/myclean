#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

struct ServiceOptions {
  std::filesystem::path database;      // the SQLite store
  std::filesystem::path data_dir;      // the build's data folder, handed to components
  std::wstring pipe_name;              // \\.\pipe\myclean, or a test pipe
  std::string owner_sid;               // the account that may use the pipe besides SYSTEM
  std::string mode;                    // "service" or "console", reported by status
  std::optional<std::string> version;  // the deploy manifest's version
};

// The running service body, independent of how it was started (SCM, console, test).
//
// start() opens the store (integrity check, migrations of the store and of every component),
// starts the components in order, registers the `status` method and opens the pipe. Any failure
// undoes the steps before it and is returned, so the service never runs half-initialized
// (contract C7). stop() closes the pipe, stops the components in reverse order and flushes and
// closes the store (contract C3).
class Service {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Service>> start(
      ServiceOptions options, std::vector<std::unique_ptr<IComponent>> components,
      const os::IClock& clock);

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  Service(Service&&) = delete;
  Service& operator=(Service&&) = delete;
  ~Service();

  // Periodic work, about once a second: the store's tick (flush, aggregation, retention). A
  // failure marks the service degraded in `status` until a later tick succeeds.
  void tick();
  // Idempotent; returns the final flush's error, if any.
  [[nodiscard]] Result<void> stop();

  [[nodiscard]] ipc::StatusReply status() const;

 private:
  Service(ServiceOptions options, std::unique_ptr<store::Store> store, const os::IClock& clock);

  [[nodiscard]] Result<void> start_components(std::vector<std::unique_ptr<IComponent>> components);
  [[nodiscard]] Result<void> open_pipe();

  ServiceOptions options_;
  const os::IClock* clock_;
  MonoTime started_;
  std::unique_ptr<store::Store> store_;
  ipc::Dispatcher methods_;
  std::vector<std::unique_ptr<IComponent>> running_;  // started components, in start order
  std::unique_ptr<platform::PipeServer> server_;
  bool stopped_ = false;

  mutable std::mutex mutex_;  // guards store_error_
  std::optional<std::string> store_error_;
};

}  // namespace myclean::svc
