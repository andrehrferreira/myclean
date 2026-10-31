#include "tray/user_tools.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/placement/drives.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/user_tasks/drive_facts.hpp"
#include "core/user_tasks/task.hpp"
#include "platform/cleanup/win32_command_runner.hpp"
#include "platform/cleanup/win32_handler_host.hpp"
#include "platform/fs/win32_file_system.hpp"
#include "platform/settings/tree_copier.hpp"
#include "platform/settings/user_settings.hpp"
#include "tray/poll_thread.hpp"

namespace myclean::tray {

namespace {

constexpr std::chrono::minutes handler_scan_timeout{2};
constexpr std::chrono::minutes handler_purge_timeout{30};

// The relocation targets as the tray last polled them, read again at each call.
class PolledDriveFacts final : public placement::IDriveFacts {
 public:
  explicit PolledDriveFacts(SharedModel& shared) : shared_(&shared) {}

  [[nodiscard]] Result<std::vector<placement::TargetVolume>> volumes() const override {
    std::vector<tray_model::VolumeRow> volumes;
    std::optional<std::vector<tray_model::DriveHealthRow>> drives;
    {
      const std::scoped_lock lock(shared_->mutex);
      if (!shared_->model.service_reachable()) {
        return fail(Errc::refused, "the service is not reachable, so no drive health is known");
      }
      volumes = shared_->model.volumes();
      drives = shared_->model.drives();
    }
    return user_tasks::target_volumes(volumes, drives);
  }

 private:
  SharedModel* shared_;
};

}  // namespace

user_tasks::TaskReport run_user_task(const user_tasks::UserTask& task, const std::stop_token& stop,
                                     SharedModel& shared) {
  platform::Win32CommandRunner runner;
  platform::Win32FileSystem file_system;
  platform::Win32UserEnvironment environment;
  platform::Win32TextFiles files;
  platform::Win32TreeCopier copier;
  const PolledDriveFacts drives(shared);
  std::unique_ptr<platform::Win32HandlerHost> handlers;
  if (task.kind == user_tasks::TaskKind::handler) {
    handlers =
        std::make_unique<platform::Win32HandlerHost>(handler_scan_timeout, handler_purge_timeout);
  }
  const user_tasks::Toolbox tools{.runner = &runner,
                                  .handlers = handlers.get(),
                                  .file_system = &file_system,
                                  .placement = {.environment = &environment,
                                                .files = &files,
                                                .copier = &copier,
                                                .file_system = &file_system,
                                                .drives = &drives}};
  return user_tasks::run(task, tools, stop);
}

}  // namespace myclean::tray
