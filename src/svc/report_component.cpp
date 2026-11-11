#include "svc/report_component.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/alerts/engine.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/reports.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/report/diff.hpp"
#include "core/report/month.hpp"
#include "core/report/render.hpp"
#include "core/report/repository.hpp"
#include "core/report/snapshot.hpp"
#include "core/sched/schedule_repository.hpp"
#include "core/store/database.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;

// The alert reading of a finished report (rule report.ready in data/alerts.toml); its entity is
// the month, which the tray's toast opens.
constexpr std::string_view ready_metric = "report.ready";

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

Result<void> write_file(const std::filesystem::path& file, std::string_view content) {
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  if (error) {
    return fail(Errc::io_error, "creating " + utf8(file.parent_path()) + ": " + error.message());
  }
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  if (!out) {
    return fail(Errc::io_error, "writing " + utf8(file) + " failed");
  }
  return {};
}

// The Markdown and HTML files of `stored`.
Result<void> write_files(const report::StoredReport& stored, std::string_view html) {
  if (Result<void> written = write_file(stored.markdown_file, stored.markdown); !written) {
    return written;
  }
  return write_file(stored.html_file, html);
}

Json months_json(const std::vector<std::chrono::year_month>& months) {
  Json list = Json::array();
  for (const std::chrono::year_month month : months) {
    list.push_back(report::month_text(month));
  }
  return list;
}

Result<std::chrono::year_month> month_param(std::string_view text) {
  const std::optional<std::chrono::year_month> month = report::parse_month(text);
  if (!month) {
    return fail(Errc::invalid_argument,
                "a month is YYYY-MM, such as 2026-11, not '" + std::string(text) + "'");
  }
  return *month;
}

}  // namespace

ReportComponent::~ReportComponent() {
  stop();
}

std::vector<store::MigrationSet> ReportComponent::migrations() const {
  return {report::report_migrations()};
}

Result<void> ReportComponent::start(const Context& context) {
  store_ = &context.store();
  clock_ = &context.clock();
  if (sources_.reports_dir.empty() && !context.state_dir().empty()) {
    sources_.reports_dir = context.state_dir() / "reports";
  }
  if (!sources_.call) {
    const ipc::Dispatcher* methods = &context.methods();
    sources_.call = [methods](std::string_view method, const Json& params) -> Result<Json> {
      return ipc::parse_response(methods->handle(ipc::make_request(1, method, params)), 1);
    };
  }
  repository_.emplace(*store_);
  scheduler_.emplace(*clock_, *repository_);
  if (Result<void> added =
          scheduler_->add(std::string(report::monthly_job), report::monthly_schedule);
      !added) {
    return added;
  }
  const Result<std::optional<sched::ScheduleState>> state = repository_->load(report::monthly_job);
  const Result<bool> done = has_snapshot(report::month_of(clock_->now(), clock_->zone()));
  if (!state || !done) {
    return tl::unexpected(state ? done.error() : state.error());
  }
  const std::optional<sched::ScheduleState>& stored = *state;
  retry_ = report::retry_at_start(stored.has_value() ? stored->last_run : std::nullopt, *done,
                                  clock_->now(), clock_->zone());

  Result<void> registered = context.methods().add(
      std::string(ipc::reports_get_method),
      {ipc::Param{.name = "month", .type = ipc::ParamType::token, .required = true}},
      [this](const Json& params) { return get(params.at("month").get<std::string>()); });
  if (registered) {
    registered = context.methods().add(
        std::string(ipc::reports_compare_method),
        {ipc::Param{.name = "a", .type = ipc::ParamType::token, .required = true},
         ipc::Param{.name = "b", .type = ipc::ParamType::token, .required = true}},
        [this](const Json& params) {
          return compare(params.at("a").get<std::string>(), params.at("b").get<std::string>());
        });
  }
  if (registered) {
    registered = context.methods().add(std::string(ipc::reports_list_method), {},
                                       [this](const Json& /*params*/) { return list(); });
  }
  if (!registered) {
    return registered;
  }
  if (sources_.poll_period.count() > 0) {
    // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  }
  return {};
}

void ReportComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

bool ReportComponent::wait(const std::stop_token& stop, std::chrono::seconds delay) {
  std::mutex wait_mutex;
  std::unique_lock wait_lock(wait_mutex);
  static_cast<void>(wake_.wait_for(wait_lock, stop, delay, [] { return false; }));
  return !stop.stop_requested();
}

void ReportComponent::run(const std::stop_token& stop) {
  if (!wait(stop, sources_.warm_up)) {
    return;
  }
  poll_now(stop);
  while (wait(stop, sources_.poll_period)) {
    poll_now(stop);
  }
}

void ReportComponent::poll_now(const std::stop_token& stop) {
  if (!scheduler_) {
    return;
  }
  const sched::PollResult due = scheduler_->poll();
  for (const Error& error : due.errors) {
    spdlog::warn("report: scheduler: {}", error.message);
  }
  bool ran = false;
  for (const sched::DueRun& run : due.runs) {
    if (run.job != report::monthly_job) {
      continue;
    }
    ran = true;
    const Result<void> outcome = produce(stop);
    if (Result<void> finished = scheduler_->finish(report::monthly_job, outcome); !finished) {
      spdlog::warn("report: scheduler: {}", finished.error().message);
    }
  }
  bool retry = false;
  {
    const std::scoped_lock lock(mutex_);
    retry = retry_ && !ran;
    retry_ = false;
  }
  if (retry) {
    spdlog::info("report: this month's run was interrupted; running it again");
    static_cast<void>(produce(stop));
  }
}

Result<bool> ReportComponent::has_snapshot(std::chrono::year_month month) const {
  bool found = false;
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<std::optional<Json>> snapshot = report::load_snapshot(db, month);
    if (!snapshot) {
      return tl::unexpected(snapshot.error());
    }
    found = snapshot->has_value();
    return {};
  });
  return read ? Result<bool>(found) : tl::unexpected(read.error());
}

Result<void> ReportComponent::produce(const std::stop_token& stop) {
  const WallTime now = clock_->now();
  const std::chrono::year_month month = report::month_of(now, clock_->zone());
  Result<void> outcome = make_report(stop, now, month);
  const std::scoped_lock lock(mutex_);
  if (outcome) {
    error_.reset();
  } else {
    error_ = "the report for " + report::month_text(month) + ": " + outcome.error().message;
    spdlog::warn("report: {}", *error_);
  }
  return outcome;
}

Result<void> ReportComponent::make_report(const std::stop_token& stop, WallTime now,
                                          std::chrono::year_month month) {
  if (sources_.reports_dir.empty()) {
    return fail(Errc::invalid_state, "no reports folder");
  }
  const Result<bool> exists = has_snapshot(month);
  if (!exists || *exists) {
    return exists ? Result<void>{} : tl::unexpected(exists.error());
  }
  std::optional<std::pair<std::chrono::year_month, Json>> previous;
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        auto found = report::newest_before(db, month);
        if (!found) {
          return tl::unexpected(found.error());
        }
        previous = std::move(*found);
        return {};
      });
      !read) {
    return read;
  }
  const std::chrono::time_zone& zone = clock_->zone();
  const WallTime period_start =
      previous ? store::from_unix_seconds(previous->second.value("taken_at", std::int64_t{0}))
               : now - report::health_days;
  const report::Probes probes{
      .call = sources_.call,
      .wait = [this, &stop](std::chrono::seconds delay) { return wait(stop, delay); },
      .store = store_,
      .poll_interval = sources_.analysis_poll,
      .max_polls = sources_.max_polls};
  const Json snapshot = report::take_snapshot(probes, now, period_start, zone);
  if (stop.stop_requested()) {
    return fail(Errc::invalid_state, "MyClean stopped before the snapshot was complete");
  }
  const Json changes = previous ? report::diff(previous->second, snapshot) : Json(nullptr);
  const std::optional<std::chrono::year_month> against =
      previous ? std::optional(previous->first) : std::nullopt;
  const report::Rendered rendered = report::render(snapshot, changes, against, zone);
  const report::StoredReport stored{
      .month = month,
      .generated_at = now,
      .against = against,
      .markdown_file = report::markdown_file(sources_.reports_dir, month),
      .html_file = report::html_file(sources_.reports_dir, month),
      .markdown = rendered.markdown,
      .diff = changes};
  if (Result<void> written = write_files(stored, rendered.html); !written) {
    return written;
  }
  if (Result<void> saved =
          store_->write([&](store::Database& db) { return report::save(db, snapshot, stored); });
      !saved) {
    return saved;
  }
  const std::string name = report::month_text(month);
  if (alerts_ != nullptr) {
    const alerts::Reading ready{
        .metric = std::string(ready_metric),
        .entity = name,
        .value = 1.0,
        .detail = "The report for " + name + " is ready: click to open it."};
    alerts_->report(std::span(&ready, 1));
  }
  spdlog::info("report: the report for {} is in {}", name, utf8(stored.html_file));
  return {};
}

Result<Json> ReportComponent::list() const {
  Json reports = Json::array();
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<std::vector<std::chrono::year_month>> months = report::stored_months(db, false);
    if (!months) {
      return tl::unexpected(months.error());
    }
    for (const std::chrono::year_month month : *months) {
      Result<std::optional<report::StoredReport>> found = report::load_report(db, month);
      if (!found) {
        return tl::unexpected(found.error());
      }
      if (!*found) {
        continue;
      }
      const report::StoredReport& stored = **found;
      reports.push_back(Json{
          {"month", report::month_text(month)},
          {"snapshot", report::month_text(month)},
          {"generated_at", store::to_unix_seconds(stored.generated_at)},
          {"against", stored.against ? Json(report::month_text(*stored.against)) : Json(nullptr)},
          {"first_report", !stored.against.has_value()},
          {"files", {{"markdown", utf8(stored.markdown_file)}, {"html", utf8(stored.html_file)}}}});
    }
    return {};
  });
  return read ? Result<Json>(Json{{"reports", std::move(reports)}}) : tl::unexpected(read.error());
}

Result<Json> ReportComponent::get(std::string_view month) const {
  const Result<std::chrono::year_month> wanted = month_param(month);
  if (!wanted) {
    return tl::unexpected(wanted.error());
  }
  Json answer;
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<std::optional<report::StoredReport>> found = report::load_report(db, *wanted);
    if (!found) {
      return tl::unexpected(found.error());
    }
    if (!*found) {
      Result<std::vector<std::chrono::year_month>> months = report::stored_months(db, false);
      if (!months) {
        return tl::unexpected(months.error());
      }
      answer = Json{{"found", false},
                    {"month", std::string(month)},
                    {"message", "no report exists for " + std::string(month)},
                    {"available", months_json(*months)}};
      return {};
    }
    const report::StoredReport& stored = **found;
    answer = Json{
        {"found", true},
        {"month", std::string(month)},
        {"generated_at", store::to_unix_seconds(stored.generated_at)},
        {"against", stored.against ? Json(report::month_text(*stored.against)) : Json(nullptr)},
        {"first_report", !stored.against.has_value()},
        {"files", {{"markdown", utf8(stored.markdown_file)}, {"html", utf8(stored.html_file)}}},
        {"markdown", stored.markdown},
        {"diff", stored.diff}};
    return {};
  });
  return read ? Result<Json>(std::move(answer)) : tl::unexpected(read.error());
}

Result<Json> ReportComponent::compare(std::string_view a, std::string_view b) const {
  const Result<std::chrono::year_month> from = month_param(a);
  const Result<std::chrono::year_month> to = month_param(b);
  if (!from || !to) {
    return tl::unexpected(from ? to.error() : from.error());
  }
  Json answer;
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<std::optional<Json>> before = report::load_snapshot(db, *from);
    Result<std::optional<Json>> after = report::load_snapshot(db, *to);
    if (!before || !after) {
      return tl::unexpected(before ? after.error() : before.error());
    }
    Json missing = Json::array();
    if (!*before) {
      missing.push_back(std::string(a));
    }
    if (!*after && a != b) {
      missing.push_back(std::string(b));
    }
    if (!missing.empty()) {
      Result<std::vector<std::chrono::year_month>> months = report::stored_months(db, true);
      if (!months) {
        return tl::unexpected(months.error());
      }
      std::string message = "no snapshot exists for " + missing[0].get<std::string>();
      if (missing.size() > 1) {
        message += " or " + missing[1].get<std::string>();
      }
      answer = Json{{"found", false},
                    {"missing", std::move(missing)},
                    {"message", std::move(message)},
                    {"available", months_json(*months)}};
      return {};
    }
    const Json changes = report::diff(**before, **after);
    answer = Json{{"found", true},
                  {"a", std::string(a)},
                  {"b", std::string(b)},
                  {"diff", changes},
                  {"markdown", report::diff_markdown(changes)}};
    return {};
  });
  return read ? Result<Json>(std::move(answer)) : tl::unexpected(read.error());
}

ipc::ComponentHealth ReportComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  const std::scoped_lock lock(mutex_);
  if (error_) {
    health.problems.push_back(*error_);
  }
  health.healthy = health.problems.empty();
  return health;
}

}  // namespace myclean::svc
