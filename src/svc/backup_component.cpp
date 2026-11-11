#include "svc/backup_component.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/alerts/engine.hpp"
#include "core/backup/backup.hpp"
#include "core/clean/workspaces.hpp"
#include "core/ipc/backup.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;

}  // namespace

BackupComponent::~BackupComponent() {
  stop();
}

Result<void> BackupComponent::start(const Context& context) {
  clock_ = &context.clock();
  data_dir_ = context.data_dir();
  if (sources_.system && !context.owner_sid().empty()) {
    system_ = sources_.system(context.owner_sid());
  }
  if (sources_.git && !context.owner_sid().empty()) {
    git_ = sources_.git(context.owner_sid());
  }
  Result<void> registered =
      context.methods().add(std::string(ipc::backups_check_method),
                            {ipc::Param{.name = "refresh", .type = ipc::ParamType::boolean}},
                            [this](const Json& params) -> Result<Json> {
                              return answer(params.value("refresh", false));
                            });
  if (!registered) {
    return registered;
  }
  if (system_ && sources_.reader && sources_.period.count() > 0) {
    {
      const std::scoped_lock lock(mutex_);
      requested_ = true;  // the first check runs at once
    }
    // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  }
  return {};
}

void BackupComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

void BackupComponent::run(const std::stop_token& stop) {
  std::mutex wait_mutex;
  std::unique_lock wait_lock(wait_mutex);
  while (!stop.stop_requested()) {
    bool due = false;
    {
      const std::scoped_lock lock(mutex_);
      due = requested_ || (checked_at_ && clock_->now() - *checked_at_ >= sources_.period);
    }
    if (due) {
      check_now(stop);
    }
    static_cast<void>(wake_.wait_for(wait_lock, stop, std::chrono::minutes{1}, [this] {
      const std::scoped_lock lock(mutex_);
      return requested_;
    }));
  }
}

void BackupComponent::check_now(const std::stop_token& stop) {
  {
    const std::scoped_lock lock(mutex_);
    requested_ = false;
    checking_ = true;
  }
  if (!system_ || !sources_.reader) {
    const std::scoped_lock lock(mutex_);
    checking_ = false;
    error_ = "the backup state cannot be read here (no owner)";
    return;
  }
  backup::Probes probes{.system = system_.get(),
                        .reader = sources_.reader.get(),
                        .drives = sources_.drives.get(),
                        .system_root = sources_.system_root};
  std::optional<std::string> problem;
  if (git_ && sources_.files != nullptr) {
    const std::filesystem::path file = data_dir_ / "workspaces.toml";
    const Result<std::string> text = sources_.files->read(file);
    Result<clean::Workspaces> workspaces =
        text ? clean::parse_workspace_file(*text, file)
             : Result<clean::Workspaces>(tl::unexpected(text.error()));
    if (workspaces) {
      probes.git = git_.get();
      probes.workspace_roots = std::move(workspaces->roots);
      probes.skip = std::move(workspaces->skip);
      probes.repo_limits.max_depth = workspaces->max_depth;
    } else {
      problem = "repositories: " + workspaces.error().message;
    }
  }
  const WallTime now = clock_->now();
  const backup::Report report = backup::check(probes, now, stop);
  if (stop.stop_requested()) {
    const std::scoped_lock lock(mutex_);
    checking_ = false;
    return;
  }
  if (alerts_ != nullptr) {
    alerts_->report(backup::age_readings(report, now));
  }
  const std::scoped_lock lock(mutex_);
  checking_ = false;
  checked_at_ = now;
  error_.reset();
  if (problem != problem_ && problem) {
    spdlog::warn("backup: {}", *problem);
  }
  problem_ = std::move(problem);
  report_ = backup::report_json(report, now);
}

Json BackupComponent::answer(bool refresh) {
  Json answer;
  bool queued = false;
  {
    const std::scoped_lock lock(mutex_);
    const bool stale = !checked_at_ ||
                       clock_->now() - *checked_at_ >= std::chrono::hours{ipc::backup_result_hours};
    if (!checking_ && !requested_ && (refresh || stale) && worker_.joinable()) {
      requested_ = true;
      queued = true;
    }
    answer = report_;
    std::string_view state = error_ ? "failed" : "ready";
    if (checking_ || requested_) {
      state = "checking";
    }
    answer["state"] = state;
    answer["checked_at"] = checked_at_ ? Json(store::to_unix_seconds(*checked_at_)) : Json(nullptr);
    answer["error"] = error_ ? Json(*error_) : Json(nullptr);
  }
  if (queued) {
    wake_.notify_all();
  }
  return answer;
}

ipc::ComponentHealth BackupComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  const std::scoped_lock lock(mutex_);
  if (error_) {
    health.problems.push_back(*error_);
  }
  if (problem_) {
    health.problems.push_back(*problem_);
  }
  health.healthy = health.problems.empty();
  return health;
}

}  // namespace myclean::svc
