#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/actions/schema.hpp"
#include "core/events/events.hpp"
#include "core/governing/governor.hpp"
#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/policy.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/schema.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/process_collector.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_jobs.hpp"

// A simulated machine for the governor: processes and job objects in FakeJobs (which is also the
// process control), the telemetry view and metrics the test sets, a store in a temporary folder,
// and the governor driven second by second on a FakeClock. Nothing touches the real OS.
namespace myclean::governing::testing {

// 2026-11-01 12:00:00 UTC.
inline const WallTime noon =
    std::chrono::sys_days{std::chrono::November / 1 / 2026} + std::chrono::hours{12};

// The file name of a UTF-8 path.
inline std::string file_name(std::string_view path) {
  const std::u8string name =
      std::filesystem::path(std::u8string(path.begin(), path.end())).filename().u8string();
  return {name.begin(), name.end()};
}

// build: cargo, rustc, msbuild, cl; test: pytest; system: svchost.
inline rules::ClassSet test_classes() {
  Result<rules::ClassSet> parsed = rules::parse_class_file(R"([[match]]
id = "test.system"
class = "system"
image = ["svchost.exe"]

[[match]]
id = "test.build"
class = "build"
image = ["cargo.exe", "rustc.exe", "msbuild.exe", "cl.exe"]

[[match]]
id = "test.test"
class = "test"
image = ["pytest.exe"]
)",
                                                           "classes.toml");
  EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
  return parsed.value_or(rules::ClassSet{});
}

// The default policy in act mode, with every ladder step open to the default class too: the
// tests drive a `default` tree (bigjob.exe) up the whole ladder.
inline governor::GovernorPolicy act_policy() {
  governor::GovernorPolicy policy;
  policy.mode = governor::GovernorMode::act;
  policy.ladder.cap = {rules::ProcessClass::build, rules::ProcessClass::test,
                       rules::ProcessClass::normal};
  policy.ladder.suspend = policy.ladder.cap;
  return policy;
}

class Machine {
 public:
  Machine() {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    folder_ = std::filesystem::temp_directory_path() /
              ("myclean-governing-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    const std::vector<store::MigrationSet> sets = {actions::action_migrations(),
                                                   governor_migrations()};
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(folder_ / "myclean.db", clock_, sets);
    if (!opened) {
      ADD_FAILURE() << opened.error().message;
      return;
    }
    store_ = std::move(*opened);
    metrics_ = {{std::string(telemetry::metric::cpu_utility), 20.0},
                {std::string(telemetry::metric::memory_available), 32.0 * gib},
                {std::string(telemetry::metric::memory_commit), 40.0 * gib},
                {std::string(telemetry::metric::memory_commit_limit), 150.0 * gib},
                {std::string(telemetry::metric::memory_hard_faults), 0.0}};
  }
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;
  Machine(Machine&&) = delete;
  Machine& operator=(Machine&&) = delete;
  ~Machine() {
    governor_.reset();
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  static constexpr double gib = 1024.0 * 1024.0 * 1024.0;

  // What the policy, game and GPU sources return; set before start() (the policy also before a
  // reload).
  [[nodiscard]] governor::GovernorPolicy& policy() { return policy_; }
  [[nodiscard]] GameScan& games() { return games_; }
  // True: the policy source fails, as with a malformed governor.toml.
  [[nodiscard]] bool& policy_fails() { return policy_fails_; }
  [[nodiscard]] telemetry::IPerfCounters*& gpu() { return gpu_; }
  // The command lines the command-line source answers, by PID (unset: none can be read).
  [[nodiscard]] std::map<std::uint32_t, std::string>& command_lines() { return command_lines_; }

  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] fakes::FakeJobs& os() { return os_; }
  // The system metrics by name (core/telemetry/metrics.hpp).
  [[nodiscard]] std::map<std::string, double, std::less<>>& metrics() { return metrics_; }

  // Starts a governor over the machine (again after crash() or stop()).
  void start() {
    governor_ = std::make_unique<Governor>(
        GovernorSources{
            .jobs = &os_,
            .processes = &os_,
            .view = [this] { return view(); },
            .metric = [this](std::string_view name) -> std::optional<double> {
              const auto found = metrics_.find(name);
              return found == metrics_.end() ? std::nullopt : std::optional(found->second);
            },
            .classes = [this]() -> Result<rules::ClassSet> { return classes_; },
            .policy = [this]() -> Result<governor::GovernorPolicy> {
              if (policy_fails_) {
                return fail(Errc::invalid_data, "governor.toml: malformed");
              }
              return policy_;
            },
            .games = [this] { return games_; },
            .image_path = [this](std::uint32_t pid,
                                 WallTime created) -> std::optional<std::filesystem::path> {
              for (const Running& process : running_) {
                if (process.key.pid == pid && process.key.created == created) {
                  return std::filesystem::path(
                      std::u8string(process.path.begin(), process.path.end()));
                }
              }
              return std::nullopt;
            },
            .command_line = [this](std::uint32_t pid, WallTime) -> std::optional<std::string> {
              const auto found = command_lines_.find(pid);
              return found == command_lines_.end() ? std::nullopt : std::optional(found->second);
            },
            .gpu = gpu_},
        *store_, clock_);
    const Result<void> started = governor_->start();
    ASSERT_TRUE(started.has_value()) << started.error().message;
  }

  // The service process dies: no stop, the governor's handles are closed.
  void crash() { governor_.reset(); }

  [[nodiscard]] Governor& governor() { return *governor_; }
  [[nodiscard]] store::Store& store() { return *store_; }

  // A running process (in every job of its parent, as Windows does), created a second after the
  // previous one, visible in the process table from the next second on. `path` is its full image
  // path; `start_event` also delivers its ETW start to the governor.
  governor::ProcessKey spawn(std::uint32_t pid, const std::string& path,
                             const std::optional<governor::ProcessKey>& parent = std::nullopt,
                             bool start_event = true, double cpu_percent = 0.0,
                             Bytes private_bytes = Bytes{}) {
    const WallTime created = noon - std::chrono::hours{1} + std::chrono::seconds{++spawned_};
    const os::ProcessKey key =
        parent ? os_.start_child(os::ProcessKey{.pid = parent->pid, .created = parent->created},
                                 pid, created)
               : os_.add_process(pid, created);
    const governor::ProcessKey process{.pid = key.pid, .created = key.created};
    running_.push_back(Running{.key = process,
                               .parent = parent,
                               .path = path,
                               .cpu_percent = cpu_percent,
                               .private_bytes = private_bytes});
    if (start_event) {
      this->start_event(process);
    }
    return process;
  }

  // Delivers the ETW start of a running process spawned without it (events queue behind the
  // governor's tick, so a start can be handled after the process started children).
  void start_event(const governor::ProcessKey& process) {
    const auto found = std::ranges::find(running_, process, &Running::key);
    ASSERT_NE(found, running_.end());
    const std::optional<governor::ProcessKey>& parent = found->parent;
    governor_->on_start(
        events::ProcessStart{.process = {.pid = process.pid, .created = process.created},
                             .parent_pid = parent ? parent->pid : 4,
                             .parent = parent ? std::optional(events::ProcessKey{
                                                    .pid = parent->pid, .created = parent->created})
                                              : std::nullopt,
                             .session = 1,
                             .image = found->path,
                             .command_line = "\"" + found->path + "\""});
  }

  // Leaves `process` out of the process table until show(): it started after the latest sample.
  void hide(const governor::ProcessKey& process) { hidden_.push_back(process); }
  void show(const governor::ProcessKey& process) { std::erase(hidden_, process); }

  // From now on the process table reports `process` in session `session` (0: a service).
  void set_session(const governor::ProcessKey& process, std::uint32_t session) {
    for (Running& running : running_) {
      if (running.key == process) {
        running.session = session;
      }
    }
  }

  // The process exits: FakeJobs posts its exit, the process table drops it, ETW reports it.
  void exit(const governor::ProcessKey& process, std::uint32_t exit_code = 0) {
    os_.exit_process(os::ProcessKey{.pid = process.pid, .created = process.created}, exit_code);
    std::erase_if(running_, [&](const Running& running) { return running.key == process; });
    if (!governor_) {
      return;  // after crash(): only the OS sees it
    }
    governor_->on_stop(events::ProcessStop{
        .process = {.pid = process.pid, .created = process.created}, .exited = clock_.now()});
  }

  // One second passes and the governor ticks.
  void second() {
    clock_.advance(std::chrono::seconds{1});
    governor_->tick();
  }
  void seconds(int count) {
    for (int second = 0; second < count; ++second) {
      this->second();
    }
  }

  // The single integer `sql` returns, or -1.
  [[nodiscard]] std::int64_t count(std::string_view sql) {
    std::int64_t value = -1;
    const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
      Result<store::Statement> query = db.prepare(sql);
      if (!query) {
        return tl::unexpected(query.error());
      }
      const Result<bool> row = query->step();
      if (row && *row) {
        value = query->column_int(0);
      }
      return {};
    });
    EXPECT_TRUE(read.has_value());
    return value;
  }

  [[nodiscard]] std::vector<GovernorEvent> events(std::size_t limit = 200) {
    const Result<std::vector<GovernorEvent>> loaded = load_events(*store_, WallTime{}, limit);
    EXPECT_TRUE(loaded.has_value());
    return loaded.value_or(std::vector<GovernorEvent>{});
  }

  [[nodiscard]] const fakes::FakeProcess& process(const governor::ProcessKey& key) const {
    return os_.process(os::ProcessKey{.pid = key.pid, .created = key.created});
  }

  // The processes of the table, as the telemetry pipeline would publish them now.
  [[nodiscard]] std::shared_ptr<const telemetry::ProcessView> view() const {
    auto table = std::make_shared<telemetry::ProcessView>();
    table->time = clock_.now();
    std::vector<std::optional<governor::ProcessKey>> parents;  // per row
    for (const Running& process : running_) {
      if (std::ranges::find(hidden_, process.key) != hidden_.end()) {
        continue;
      }
      telemetry::ProcessRow row{.info = {.pid = process.key.pid,
                                         .parent_pid = process.parent ? process.parent->pid : 4,
                                         .created = process.key.created,
                                         .image = file_name(process.path),
                                         .private_bytes = process.private_bytes,
                                         .session = process.session},
                                .cpu_percent = process.cpu_percent};
      table->rows.push_back(std::move(row));
      parents.push_back(process.parent);
    }
    for (std::size_t child = 0; child < table->rows.size(); ++child) {
      const std::optional<governor::ProcessKey>& parent = parents[child];
      if (!parent) {
        continue;
      }
      for (std::size_t index = 0; index < table->rows.size(); ++index) {
        const telemetry::ProcessInfo& info = table->rows[index].info;
        if (info.pid == parent->pid && info.created == parent->created) {
          table->rows[child].parent = index;
        }
      }
    }
    return table;
  }

 private:
  struct Running {
    governor::ProcessKey key;
    std::optional<governor::ProcessKey> parent;
    std::string path;
    double cpu_percent = 0.0;
    Bytes private_bytes;
    std::optional<std::uint32_t> session;  // as the process table reports it
  };

  governor::GovernorPolicy policy_ = act_policy();
  rules::ClassSet classes_ = test_classes();
  GameScan games_;
  bool policy_fails_ = false;
  telemetry::IPerfCounters* gpu_ = nullptr;
  std::map<std::uint32_t, std::string> command_lines_;
  fakes::FakeClock clock_{noon};
  fakes::FakeJobs os_{true};
  std::map<std::string, double, std::less<>> metrics_;
  std::filesystem::path folder_;
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<Governor> governor_;
  std::vector<Running> running_;
  std::vector<governor::ProcessKey> hidden_;  // left out of the process table
  int spawned_ = 0;
};

// The persisted JobRef of every tracked job of `role`, from governor_job.
inline std::vector<JobRecord> jobs_of(store::Store& store, governor::JobRole role) {
  const Result<std::vector<JobRecord>> jobs = load_jobs(store);
  EXPECT_TRUE(jobs.has_value());
  std::vector<JobRecord> matching;
  for (const JobRecord& job : jobs.value_or(std::vector<JobRecord>{})) {
    if (job.role == role) {
      matching.push_back(job);
    }
  }
  return matching;
}

}  // namespace myclean::governing::testing
