#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/backup/backup.hpp"
#include "core/backup/repos.hpp"
#include "core/health/analysis.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/rules/data_files.hpp"
#include "core/space/scanner.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

struct BackupSources {
  // The backup state of the owner with this SID; null: no check runs.
  std::function<std::unique_ptr<backup::IBackupSystem>(std::string_view owner_sid)> system;
  std::unique_ptr<space::IDirectoryReader> reader;
  std::unique_ptr<placement::IDriveFacts> drives;  // null: no recommendation
  // git as the owner with this SID (git refuses another user's repositories); null or empty: no
  // repository check.
  std::function<std::unique_ptr<backup::IRepoGit>(std::string_view owner_sid)> git;
  std::filesystem::path system_root{L"C:\\"};
  // Reads data\workspaces.toml (the repository roots); null: no repository check.
  const rules::IDataFiles* files = nullptr;
  // How often the check runs on its own; zero: no thread (tests call check_now()).
  std::chrono::hours period{24};
};

// The backup check (core/backup) on a thread of its own: at the start, every `period` and on a
// backups.check request with refresh. Each check's age reading goes to `alerts` (rule backup.age
// in data/alerts.toml), so a stale or missing backup raises one rate-limited, snoozable alert.
// The repository roots and the folders never entered are those of data/workspaces.toml. It only
// reads: nothing configures a backup or writes to a repository. Registers backups.check
// (core/ipc/backup.hpp).
class BackupComponent final : public IComponent {
 public:
  // `alerts` (may be null) must outlive the component.
  explicit BackupComponent(BackupSources sources, health::IAlertSink* alerts = nullptr)
      : sources_(std::move(sources)), alerts_(alerts) {}
  BackupComponent(const BackupComponent&) = delete;
  BackupComponent& operator=(const BackupComponent&) = delete;
  BackupComponent(BackupComponent&&) = delete;
  BackupComponent& operator=(BackupComponent&&) = delete;
  ~BackupComponent() override;

  [[nodiscard]] std::string_view name() const override { return "backup"; }
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // The backups.check answer; queues a check when `refresh` or the last one is stale.
  [[nodiscard]] nlohmann::json answer(bool refresh);
  // Runs one check now on the calling thread (the thread's work; tests without a thread).
  void check_now(const std::stop_token& stop);

 private:
  void run(const std::stop_token& stop);

  BackupSources sources_;
  health::IAlertSink* alerts_;
  const os::IClock* clock_ = nullptr;
  std::filesystem::path data_dir_;
  std::unique_ptr<backup::IBackupSystem> system_;
  std::unique_ptr<backup::IRepoGit> git_;
  std::jthread worker_;
  std::condition_variable_any wake_;
  mutable std::mutex mutex_;  // guards the members below
  bool requested_ = false;
  bool checking_ = false;
  std::optional<WallTime> checked_at_;
  std::optional<std::string> error_;
  std::optional<std::string> problem_;  // data/workspaces.toml could not be read
  nlohmann::json report_ = nlohmann::json::object();
};

}  // namespace myclean::svc
