#include "svc/cleaning_component.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/actions/schema.hpp"
#include "core/cleaning/executor.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleaning/service.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

CleaningComponent::CleaningComponent(CleaningSources sources, const packs::IPackFiles& files,
                                     CleaningMode mode)
    : sources_(std::move(sources)), files_(&files), mode_(std::move(mode)) {}

CleaningComponent::~CleaningComponent() {
  stop();
}

std::vector<store::MigrationSet> CleaningComponent::migrations() const {
  return {actions::action_migrations(), cleaning::cleaning_migrations()};
}

Result<void> CleaningComponent::start(const Context& context) {
  const bool execute = !mode_.console || mode_.sandbox.has_value();
  cleaning::ServiceSettings settings{
      .executor = {.system_root = mode_.system_root, .system_changes = !mode_.console},
      .execute = execute};
  if (mode_.sandbox) {
    settings.executor.quarantine_root = *mode_.sandbox / "quarantine";
  }
  if (sources_.machine) {
    machine_ = sources_.machine(context.owner_sid());
  }
  if (sources_.bin && !context.owner_sid().empty()) {
    bin_ = sources_.bin(context.owner_sid());
  }
  if (sources_.startup && !context.owner_sid().empty()) {
    startup_ = sources_.startup(context.owner_sid());
  }
  cleaning::ServiceSources sources{
      .store = &context.store(),
      .clock = &context.clock(),
      .file_system = sources_.file_system.get(),
      .probes = {.tree = sources_.tree.get(),
                 .usage = sources_.usage.get(),
                 .git = sources_.git.get(),
                 .handlers = sources_.handlers.get(),
                 .bin = bin_.get(),
                 .commands = sources_.commands.get(),
                 .dism = sources_.dism,
                 .system = sources_.system.get(),
                 .tools = sources_.tools.get(),
                 .drives = sources_.drives.get(),
                 .startup = startup_.get(),
                 .indexer = sources_.indexer.get()},
      .owner = [owner = sources_.owner,
                sid = context.owner_sid()]() -> Result<cleaning::OwnerContext> {
        if (!owner) {
          return fail(Errc::not_found, "no owner session source");
        }
        return owner(sid);
      },
      .files = files_,
      .data_dir = mode_.sandbox ? *mode_.sandbox / "data" : context.data_dir(),
      .machine = machine_.get(),
      .owner_packs = mode_.sandbox ? decltype(sources_.owner_packs){} : sources_.owner_packs,
      .space = {.reader = sources_.reader.get(),
                .zones = sources_.zones.get(),
                .hasher = sources_.hasher.get(),
                .installers = sources_.installers.get(),
                .images = sources_.images.get()},
      .busy = sources_.busy,
      .diagnostics = sources_.diagnostics.get()};
  service_ = std::make_unique<cleaning::CleaningService>(std::move(sources), std::move(settings));
  service_->on_work([this] { wake(); });
  if (Result<void> started = service_->start(context.methods()); !started) {
    return started;
  }
  spdlog::info("cleaning: {}{}",
               execute ? "plans run"
                       : "scans and proposals only (console run "
                         "without --sandbox)",
               mode_.sandbox ? " in the sandbox" : "");
  worker_ = std::jthread([this](const std::stop_token& stop) { loop(stop, false); });
  space_worker_ = std::jthread([this](const std::stop_token& stop) { loop(stop, true); });
  return {};
}

void CleaningComponent::wake() {
  {
    const std::scoped_lock lock(mutex_);
    ++wakes_;
  }
  woken_.notify_all();
}

void CleaningComponent::loop(const std::stop_token& stop, bool space) {
  int seen = 0;
  while (!stop.stop_requested()) {
    if (space) {
      service_->work_space(stop);
    } else {
      service_->work(stop);
    }
    std::unique_lock lock(mutex_);
    woken_.wait_for(lock, stop, poll_interval, [&] { return wakes_ != seen; });
    seen = wakes_;
  }
}

void CleaningComponent::stop() {
  worker_.request_stop();
  space_worker_.request_stop();
  woken_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  if (space_worker_.joinable()) {
    space_worker_.join();
  }
}

ipc::ComponentHealth CleaningComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  if (service_) {
    health.problems = service_->problems();
    if (const std::optional<std::string> reason = service_->unavailable(); reason) {
      health.unavailable.push_back("cleaning: " + *reason);
    }
  }
  health.healthy = health.problems.empty();
  return health;
}

std::vector<ipc::PackStatus> CleaningComponent::pack_status() const {
  return service_ ? service_->pack_status() : std::vector<ipc::PackStatus>{};
}

}  // namespace myclean::svc
