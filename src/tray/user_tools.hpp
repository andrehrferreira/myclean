#pragma once

#include <stop_token>

#include "core/user_tasks/task.hpp"
#include "tray/poll_thread.hpp"

namespace myclean::tray {

// Runs `task` with the Win32 adapters, under this process's token (the owner's): commands as
// hidden child processes, the disk-cleanup handlers on a private desktop (created only for a
// handler task), the shell's Recycle Bin on another (only for a recycle_bin task), the user
// environment, the owner's registry, tool configuration files, folder copies and the process list
// (what a relocation recipe needs not running); for an archive the folders of running programs
// and the deny-list's places of the owner, for an uninstall the shell that starts uninstallers.
// Relocation targets are judged from the volumes and disk health in `shared` (what the poll thread
// last read from the service). The TaskWorker's Execute.
[[nodiscard]] user_tasks::TaskReport run_user_task(const user_tasks::UserTask& task,
                                                   const std::stop_token& stop,
                                                   SharedModel& shared);

}  // namespace myclean::tray
