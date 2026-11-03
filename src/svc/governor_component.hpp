#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "core/events/event_router.hpp"
#include "core/events/events.hpp"
#include "core/governing/governor.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/packs/loader.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/process_collector.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The OS sources of the governor; Win32 in the service, fakes in tests. Only jobs and processes
// are required.
struct GovernorSources {
  std::unique_ptr<os::IJobControl> jobs;
  std::unique_ptr<os::IProcessControl> processes;
  std::unique_ptr<telemetry::IPerfCounters> gpu;  // GPU engine counters of the fullscreen signal
  // The telemetry component's newest process table and system metrics (no query of its own).
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  std::function<std::optional<double>(std::string_view metric)> metric;
  // The launchers and GameConfigStore of the owner with this SID.
  std::function<governing::GameScan(std::string_view owner_sid)> games;
  // The full image path of a running process.
  std::function<std::optional<std::filesystem::path>(std::uint32_t pid, WallTime created)>
      image_path;
  // The command line of a running process (orphaned polling loops).
  std::function<std::optional<std::string>(std::uint32_t pid, WallTime created)> command_line;
  // The owner's override packs folder (platform/packs); empty: no owner packs.
  std::function<Result<std::optional<std::filesystem::path>>()> owner_packs;
};

// The resource governor as a service component (governing::Governor). It subscribes to the
// events component's process starts and stops, which a thread of its own handles at once (a
// build root must be in its job before it spawns children), and ticks the governor every second
// on the telemetry component's samples. Policy: <data>\governor.toml; classes: the classes of
// every loaded knowledge pack, reloaded with the policy (a reload with a rejected pack keeps the
// previous classes). Registers the methods of core/ipc/governor.hpp. stop() fails open: every cap
// is lifted and every suspend resumed before it returns.
class GovernorComponent final : public IComponent {
 public:
  static constexpr std::chrono::seconds tick_interval{1};
  // Process events waiting for the governor thread; more are dropped (and counted).
  static constexpr std::size_t max_queued_events = 4096;

  // `files` and `router` (may be null: no process events) must outlive the component.
  GovernorComponent(GovernorSources sources, const packs::IPackFiles& files,
                    events::EventRouter* router);
  GovernorComponent(const GovernorComponent&) = delete;
  GovernorComponent& operator=(const GovernorComponent&) = delete;
  GovernorComponent(GovernorComponent&&) = delete;
  GovernorComponent& operator=(GovernorComponent&&) = delete;
  ~GovernorComponent() override;

  [[nodiscard]] std::string_view name() const override { return "governor"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // The governor; null before start(). For tests.
  [[nodiscard]] governing::Governor* governor() { return governor_.get(); }

 private:
  using Event = std::variant<events::ProcessStart, events::ProcessStop>;

  // Copies process events into the queue on the event source's thread.
  class Sink final : public events::IEventSink {
   public:
    explicit Sink(GovernorComponent& owner) : owner_(&owner) {}
    void on_start(const events::ProcessStart& event) override { owner_->enqueue(event); }
    void on_stop(const events::ProcessStop& event) override { owner_->enqueue(event); }
    void on_network(const events::NetworkTransfer& /*transfer*/) override {}

   private:
    GovernorComponent* owner_;
  };

  void enqueue(Event event);
  void run(const std::stop_token& stop);
  [[nodiscard]] Result<rules::ClassSet> load_classes(const std::filesystem::path& data_dir);

  GovernorSources sources_;
  const packs::IPackFiles* files_;
  events::EventRouter* router_;
  Sink sink_{*this};
  bool subscribed_ = false;
  const os::IClock* clock_ = nullptr;
  std::unique_ptr<governing::Governor> governor_;
  std::jthread thread_;

  mutable std::mutex queue_mutex_;  // guards the members below up to packs_mutex_
  std::condition_variable_any queued_;
  std::deque<Event> queue_;
  std::uint64_t dropped_ = 0;

  mutable std::mutex packs_mutex_;  // guards the members below
  bool classes_loaded_ = false;
  std::vector<std::string> pack_problems_;
};

}  // namespace myclean::svc
