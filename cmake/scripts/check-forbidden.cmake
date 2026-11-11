# Fails when the tree holds a kernel driver or code that registers a scheduled task (PRD NFR-SEC-2:
# no kernel driver; NFR-OPS-1: scheduling lives in the app, never in the OS). It checks:
#   - no .sys file anywhere in the code, data or script folders;
#   - no driver install in code or scripts: a kernel or file-system driver service, pnputil,
#     DiInstallDriver, DriverPackageInstall, InstallHinfSection;
#   - no task registration: schtasks, the ScheduledTasks cmdlets that create or change a task, and
#     the Task Scheduler COM calls that create, register or delete one;
#   - the Task Scheduler COM service (CLSID_TaskScheduler, ITaskService) only in the startup
#     toggle, src/platform/startup, which reads tasks and flips an existing task's Enabled flag
#     (IRegisteredTask::put_Enabled) and nothing else.
# Markdown is not checked: documents may name what is forbidden.
#
# Usage: cmake -DSOURCE_DIR=<repo> -P check-forbidden.cmake

cmake_minimum_required(VERSION 3.25)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is not set")
endif()

set(checked_dirs src tests tools cmake data .github .githooks)
set(checked_extensions cpp hpp h ipp cmake txt ps1 psm1 psd1 bat cmd yml yaml toml json)

# Patterns, matched case-insensitively against each line (CMake regular expressions).
set(driver_patterns
  "SERVICE_KERNEL_DRIVER" "SERVICE_FILE_SYSTEM_DRIVER" "type= *kernel" "type= *filesys"
  "-Type +Kernel" "pnputil" "DiInstallDriver" "DriverPackageInstall" "InstallHinfSection")
set(task_patterns
  "schtasks" "Register-ScheduledTask" "New-ScheduledTask" "Set-ScheduledTask"
  "Unregister-ScheduledTask" "RegisterTaskDefinition" "RegisterTask[ \t]*\\(" "NewTask[ \t]*\\("
  "DeleteTask[ \t]*\\(" "CreateFolder[ \t]*\\(" "ITaskFolder::Register")
set(task_service_pattern "CLSID_TaskScheduler|ITaskService")
set(task_service_home "src/platform/startup/")
set(self "cmake/scripts/check-forbidden.cmake")

set(globs "")
foreach(dir IN LISTS checked_dirs)
  list(APPEND globs "${SOURCE_DIR}/${dir}/*")
endforeach()
file(GLOB_RECURSE files ${globs})
file(GLOB top_files "${SOURCE_DIR}/CMakeLists.txt" "${SOURCE_DIR}/CMakePresets.json"
  "${SOURCE_DIR}/vcpkg.json")
list(APPEND files ${top_files})
list(SORT files)

set(violations "")
foreach(file IN LISTS files)
  cmake_path(RELATIVE_PATH file BASE_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE relative)
  cmake_path(GET file EXTENSION LAST_ONLY extension)
  string(TOLOWER "${extension}" extension)
  if(extension STREQUAL ".sys")
    string(APPEND violations "  ${relative}: a driver file (.sys)\n")
    continue()
  endif()
  set(bare "")
  if(extension)
    string(SUBSTRING "${extension}" 1 -1 bare)
  endif()
  if(relative STREQUAL self OR NOT bare IN_LIST checked_extensions)
    continue()
  endif()
  file(STRINGS "${file}" lines)
  set(line_number 0)
  foreach(line IN LISTS lines)
    math(EXPR line_number "${line_number} + 1")
    string(TOLOWER "${line}" lowered)
    foreach(kind IN ITEMS driver task)
      foreach(pattern IN LISTS ${kind}_patterns)
        string(TOLOWER "${pattern}" lowered_pattern)
        if(lowered MATCHES "${lowered_pattern}")
          string(APPEND violations "  ${relative}:${line_number}: ${kind} (${pattern}): ${line}\n")
        endif()
      endforeach()
    endforeach()
    string(FIND "${relative}" "${task_service_home}" in_home)
    if(line MATCHES "${task_service_pattern}" AND NOT in_home EQUAL 0)
      string(APPEND violations
        "  ${relative}:${line_number}: the Task Scheduler service outside ${task_service_home}: ${line}\n")
    endif()
  endforeach()
endforeach()

if(violations)
  message(FATAL_ERROR "Forbidden driver or scheduled-task code (PRD NFR-SEC-2, NFR-OPS-1):\n"
    "${violations}"
    "MyClean installs no driver and registers no scheduled task: scheduling lives in the app "
    "(src/core/sched). See cmake/scripts/check-forbidden.cmake.")
endif()
