#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health/analysis.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/report/snapshot.hpp"
#include "core/sched/scheduler.hpp"
#include "core/sched/store_schedule_repository.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

struct ReportSources {
  // Calls a read method in-process; null: the service's own methods (Context::methods()).
  report::Call call;
  // The reports folder; empty: <state folder>\reports (%ProgramData%\MyClean\reports).
  std::filesystem::path reports_dir;
  // After the start, before the first poll: the collectors take their first readings (disk
  // health needs a minute) and every component has registered its methods.
  std::chrono::seconds warm_up{std::chrono::minutes{5}};
  // How often the scheduler is polled; zero: no thread (tests call poll_now()).
  std::chrono::seconds poll_period{30};
  // A running storage analysis is asked again this often, at most max_polls times.
  std::chrono::seconds analysis_poll{15};
  int max_polls = 240;
};

// The monthly report (reports spec; core/report) on MyClean's own scheduler: job
// "monthly-report", the 1st of every month at 09:00 local time, caught up once at the next start
// when MyClean was off (core/sched), and again at the next start when a run of this month was
// interrupted. A month gets one report at most: a month that has a snapshot never runs again.
// The job takes the snapshot through the service's read methods, compares it with the newest
// earlier snapshot, writes <reports>\YYYY-MM\report.md and report.html, stores the snapshot and
// the report, and reports "report.ready" for the month to `alerts`, whose toast opens the HTML in
// the tray. It only reads: it calls no plan or proposal method, writes nothing to action_log and
// sends nothing off the machine. Registers reports.list, reports.get and reports.compare
// (core/ipc/reports.hpp). Register it last, so every method it calls exists.
class ReportComponent final : public IComponent {
 public:
  // `alerts` (may be null) must outlive the component.
  explicit ReportComponent(ReportSources sources, health::IAlertSink* alerts = nullptr)
      : sources_(std::move(sources)), alerts_(alerts) {}
  ReportComponent(const ReportComponent&) = delete;
  ReportComponent& operator=(const ReportComponent&) = delete;
  ReportComponent(ReportComponent&&) = delete;
  ReportComponent& operator=(ReportComponent&&) = delete;
  ~ReportComponent() override;

  [[nodiscard]] std::string_view name() const override { return "report"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // One poll of the scheduler on the calling thread (the thread's work; tests without a thread):
  // runs the monthly job when it is due, or when a run of this month was interrupted.
  void poll_now(const std::stop_token& stop);
  // Produces the report of the current month now, unless that month has one already.
  [[nodiscard]] Result<void> produce(const std::stop_token& stop);

  [[nodiscard]] Result<nlohmann::json> list() const;
  [[nodiscard]] Result<nlohmann::json> get(std::string_view month) const;
  [[nodiscard]] Result<nlohmann::json> compare(std::string_view a, std::string_view b) const;

 private:
  void run(const std::stop_token& stop);
  [[nodiscard]] bool wait(const std::stop_token& stop, std::chrono::seconds delay);
  [[nodiscard]] Result<bool> has_snapshot(std::chrono::year_month month) const;
  [[nodiscard]] Result<void> make_report(const std::stop_token& stop, WallTime now,
                                         std::chrono::year_month month);

  ReportSources sources_;
  health::IAlertSink* alerts_;
  store::Store* store_ = nullptr;
  const os::IClock* clock_ = nullptr;
  std::optional<sched::StoreScheduleRepository> repository_;
  std::optional<sched::Scheduler> scheduler_;
  std::jthread worker_;
  std::condition_variable_any wake_;
  mutable std::mutex mutex_;  // guards the members below
  bool retry_ = false;        // a run of this month was interrupted: run once at the first poll
  std::optional<std::string> error_;
};

}  // namespace myclean::svc
