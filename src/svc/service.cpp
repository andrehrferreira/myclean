#include "svc/service.hpp"

#include <chrono>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <windows.h>

#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

}  // namespace

Result<std::unique_ptr<Service>> Service::start(ServiceOptions options,
                                                std::vector<std::unique_ptr<IComponent>> components,
                                                const os::IClock& clock) {
  std::vector<store::MigrationSet> migrations;
  for (const std::unique_ptr<IComponent>& component : components) {
    std::vector<store::MigrationSet> own = component->migrations();
    migrations.insert(migrations.end(), std::make_move_iterator(own.begin()),
                      std::make_move_iterator(own.end()));
  }
  Result<std::unique_ptr<store::Store>> store =
      store::Store::open(options.database, clock, migrations);
  if (!store) {
    return tl::unexpected(store.error());
  }
  spdlog::info("store {} open; integrity check passed", utf8(options.database));

  std::unique_ptr<Service> service(new Service(std::move(options), std::move(*store), clock));
  Result<void> started = service->start_components(std::move(components)).and_then([&service] {
    return service->open_pipe();
  });
  if (!started) {
    static_cast<void>(service->stop());
    return tl::unexpected(started.error());
  }
  return service;
}

Service::Service(ServiceOptions options, std::unique_ptr<store::Store> store,
                 const os::IClock& clock)
    : options_(std::move(options)),
      clock_(&clock),
      started_(clock.monotonic()),
      store_(std::move(store)) {}

Service::~Service() {
  static_cast<void>(stop());
}

Result<void> Service::start_components(std::vector<std::unique_ptr<IComponent>> components) {
  const Context context(*store_, methods_, *clock_, options_.data_dir);
  for (std::unique_ptr<IComponent>& component : components) {
    if (Result<void> started = component->start(context); !started) {
      return fail(started.error().code, "component " + std::string(component->name()) +
                                            " failed to start: " + started.error().message);
    }
    spdlog::info("component {} started", component->name());
    running_.push_back(std::move(component));
  }
  return {};
}

Result<void> Service::open_pipe() {
  if (Result<void> added = methods_.add(std::string(ipc::status_method), {},
                                        [this](const nlohmann::json&) -> Result<nlohmann::json> {
                                          return ipc::to_json(status());
                                        });
      !added) {
    return added;
  }
  Result<std::unique_ptr<platform::PipeServer>> server =
      platform::PipeServer::start(platform::PipeServerOptions{
          .name = options_.pipe_name,
          .owner_sid = options_.owner_sid,
          .handler = [this](std::string_view request) { return methods_.handle_frame(request); },
          .on_error = [](const Error& error) { spdlog::error("pipe: {}", error.message); }});
  if (!server) {
    return tl::unexpected(server.error());
  }
  server_ = std::move(*server);
  spdlog::info("pipe open for SYSTEM and {}", options_.owner_sid);
  return {};
}

void Service::tick() {
  const Result<void> ticked = store_->tick();
  const std::scoped_lock lock(mutex_);
  if (!ticked) {
    if (store_error_ != ticked.error().message) {
      spdlog::error("store: {}", ticked.error().message);
    }
    store_error_ = ticked.error().message;
  } else if (store_error_) {
    spdlog::info("store writes recovered");
    store_error_.reset();
  }
}

Result<void> Service::stop() {
  if (stopped_) {
    return {};
  }
  stopped_ = true;
  if (server_) {
    server_->stop();
    server_.reset();
  }
  for (const std::unique_ptr<IComponent>& component : std::ranges::reverse_view(running_)) {
    component->stop();
    spdlog::info("component {} stopped", component->name());
  }
  running_.clear();
  Result<void> closed = store_->close();
  if (closed) {
    spdlog::info("store flushed and closed");
  } else {
    spdlog::error("store close: {}", closed.error().message);
  }
  return closed;
}

ipc::StatusReply Service::status() const {
  std::vector<std::string> components;
  std::vector<ipc::ComponentHealth> health;
  components.reserve(running_.size());
  health.reserve(running_.size());
  bool healthy = true;
  for (const std::unique_ptr<IComponent>& component : running_) {
    components.emplace_back(component->name());
    health.push_back(component->health());
    healthy = healthy && health.back().healthy;
  }
  const std::scoped_lock lock(mutex_);
  return ipc::StatusReply{
      .healthy = healthy && !store_error_.has_value(),
      .version = options_.version,
      .build = MYCLEAN_VERSION,
      .mode = options_.mode,
      .pid = static_cast<std::int64_t>(::GetCurrentProcessId()),
      .uptime_seconds =
          std::chrono::duration_cast<std::chrono::seconds>(clock_->monotonic() - started_).count(),
      .store = ipc::StoreStatus{.path = utf8(options_.database), .error = store_error_},
      .components = std::move(components),
      .component_health = std::move(health)};
}

}  // namespace myclean::svc
