#include "svc/governor_component.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/actions/schema.hpp"
#include "core/events/events.hpp"
#include "core/governing/governor.hpp"
#include "core/governing/methods.hpp"
#include "core/governing/records.hpp"
#include "core/governor/policy.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "core/store/migrations.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

GovernorComponent::GovernorComponent(GovernorSources sources, const packs::IPackFiles& files,
                                     events::EventRouter* router)
    : sources_(std::move(sources)), files_(&files), router_(router) {}

GovernorComponent::~GovernorComponent() {
  stop();
}

std::vector<store::MigrationSet> GovernorComponent::migrations() const {
  return {actions::action_migrations(), governing::governor_migrations()};
}

Result<rules::ClassSet> GovernorComponent::load_classes(const std::filesystem::path& data_dir) {
  const packs::LoadReport report = packs::load_installed(
      *files_, data_dir,
      sources_.owner_packs ? sources_.owner_packs()
                           : Result<std::optional<std::filesystem::path>>(std::nullopt));
  std::vector<std::string> problems;
  problems.reserve(report.errors.size());
  for (const Error& error : report.errors) {
    problems.push_back("knowledge packs: " + error.message);
  }
  const std::scoped_lock lock(packs_mutex_);
  pack_problems_ = problems;
  // The first load takes what loaded: there are no previous classes to keep. Later, a rejected
  // pack (a malformed classes.toml, say) keeps the classes in effect.
  if (classes_loaded_ && !problems.empty()) {
    return fail(Errc::invalid_data, problems.front());
  }
  classes_loaded_ = true;
  return packs::make_registry(report.packs).classes;
}

Result<void> GovernorComponent::start(const Context& context) {
  if (sources_.jobs == nullptr || sources_.processes == nullptr) {
    return fail(Errc::invalid_argument, "the governor needs job and process control");
  }
  if (Result<void> retention = context.store().add_retention(governing::governor_retention());
      !retention) {
    return retention;
  }
  clock_ = &context.clock();
  governing::GovernorSources sources{
      .jobs = sources_.jobs.get(),
      .processes = sources_.processes.get(),
      .view = sources_.view,
      .metric = sources_.metric,
      .classes = [this, data_dir = context.data_dir()] { return load_classes(data_dir); },
      .policy =
          [data_dir = context.data_dir()] {
            return governor::load_policy(rules::DiskDataFiles{}, data_dir);
          },
      .image_path = sources_.image_path,
      .command_line = sources_.command_line,
      .gpu = sources_.gpu.get()};
  if (sources_.games) {
    sources.games = [games = sources_.games, owner = context.owner_sid()] { return games(owner); };
  }
  governor_ =
      std::make_unique<governing::Governor>(std::move(sources), context.store(), context.clock());
  if (Result<void> started = governor_->start(); !started) {
    return started;
  }
  if (Result<void> registered =
          governing::register_methods(context.methods(), *governor_, context.store());
      !registered) {
    governor_->stop();
    return registered;
  }
  if (router_ != nullptr) {
    router_->subscribe(sink_);
    subscribed_ = true;
  }
  thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  return {};
}

void GovernorComponent::stop() {
  if (subscribed_) {
    router_->unsubscribe(sink_);
    subscribed_ = false;
  }
  if (thread_.joinable()) {
    thread_.request_stop();
    queued_.notify_all();
    thread_.join();
  }
  if (governor_) {
    governor_->stop();
  }
}

void GovernorComponent::enqueue(Event event) {
  {
    const std::scoped_lock lock(queue_mutex_);
    if (queue_.size() >= max_queued_events) {
      ++dropped_;
      return;
    }
    queue_.push_back(std::move(event));
  }
  queued_.notify_all();
}

void GovernorComponent::run(const std::stop_token& stop) {
  MonoTime next_tick = clock_->monotonic();
  std::deque<Event> batch;
  while (!stop.stop_requested()) {
    const MonoTime now = clock_->monotonic();
    if (now >= next_tick) {
      governor_->tick();
      next_tick = now + tick_interval;
    }
    {
      // Never wait longer than a tick, so a test clock that does not move still ticks.
      const MonoTime::duration wait =
          std::clamp(next_tick - clock_->monotonic(), MonoTime::duration::zero(),
                     std::chrono::duration_cast<MonoTime::duration>(tick_interval));
      std::unique_lock lock(queue_mutex_);
      static_cast<void>(queued_.wait_for(lock, stop, wait, [this] { return !queue_.empty(); }));
      batch.swap(queue_);
    }
    for (const Event& event : batch) {
      if (const auto* started = std::get_if<events::ProcessStart>(&event); started != nullptr) {
        governor_->on_start(*started);
      } else {
        governor_->on_stop(std::get<events::ProcessStop>(event));
      }
    }
    batch.clear();
  }
}

ipc::ComponentHealth GovernorComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  if (governor_) {
    health.problems = governor_->problems();
  }
  {
    const std::scoped_lock lock(packs_mutex_);
    health.problems.insert(health.problems.end(), pack_problems_.begin(), pack_problems_.end());
  }
  {
    const std::scoped_lock lock(queue_mutex_);
    if (dropped_ > 0) {
      health.problems.push_back(std::to_string(dropped_) +
                                " process events dropped: the governor thread fell behind");
    }
  }
  if (router_ == nullptr) {
    health.unavailable.emplace_back("process events: none, so no proactive build/test jobs");
  }
  if (sources_.gpu == nullptr) {
    health.unavailable.emplace_back("GPU engine counters: no fullscreen game detection");
  }
  health.healthy = health.problems.empty();
  return health;
}

}  // namespace myclean::svc
