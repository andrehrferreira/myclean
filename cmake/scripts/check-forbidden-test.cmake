# The regression guard's own test: check-forbidden.cmake passes on the repository, and fails on a
# scratch tree with one of each forbidden thing, naming every one. The forbidden words are built
# from pieces here, so this file does not trip the guard itself.
#
# Usage: cmake -DSOURCE_DIR=<repo> -DWORK_DIR=<scratch folder> -P check-forbidden-test.cmake

cmake_minimum_required(VERSION 3.25)

if(NOT SOURCE_DIR OR NOT WORK_DIR)
  message(FATAL_ERROR "SOURCE_DIR and WORK_DIR must be set")
endif()

set(script "${SOURCE_DIR}/cmake/scripts/check-forbidden.cmake")

execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}" -P "${script}"
  RESULT_VARIABLE result ERROR_VARIABLE output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the repository fails the guard:\n${output}")
endif()

set(tree "${WORK_DIR}/check-forbidden-tree")
file(REMOVE_RECURSE "${tree}")
set(tasks "sch" "tasks")
string(JOIN "" tasks ${tasks})
set(register "Register-" "Scheduled" "Task")
string(JOIN "" register ${register})
set(definition "Register" "TaskDefinition")
string(JOIN "" definition ${definition})
set(driver "SERVICE_" "KERNEL_" "DRIVER")
string(JOIN "" driver ${driver})
set(service "CLSID_" "TaskScheduler")
string(JOIN "" service ${service})
file(WRITE "${tree}/tools/install.ps1" "${tasks} /Create /TN MyClean\n${register} -TaskName x\n")
file(WRITE "${tree}/src/platform/other/tasks.cpp"
  "folder->${definition}(name, task, 6, {}, {}, 3, {}, &out);\nCoCreateInstance(${service});\n")
file(WRITE "${tree}/src/platform/startup/win32_startup.cpp" "CoCreateInstance(${service});\n")
file(WRITE "${tree}/src/svc/driver.cpp" "CreateServiceW(m, n, n, 0, ${driver}, 0, 0, p);\n")
file(WRITE "${tree}/data/drivers/filter.sys" "")

execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${tree}" -P "${script}"
  RESULT_VARIABLE result ERROR_VARIABLE output)
file(REMOVE_RECURSE "${tree}")
if(result EQUAL 0)
  message(FATAL_ERROR "the guard passed a tree with forbidden code")
endif()
foreach(expected IN ITEMS "tools/install.ps1:1:" "tools/install.ps1:2:"
                          "src/platform/other/tasks.cpp:1:" "src/platform/other/tasks.cpp:2:"
                          "src/svc/driver.cpp:1:" "data/drivers/filter.sys")
  string(FIND "${output}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "the guard did not name ${expected}:\n${output}")
  endif()
endforeach()
string(FIND "${output}" "src/platform/startup/win32_startup.cpp" found)
if(NOT found EQUAL -1)
  message(FATAL_ERROR "the guard refused the startup toggle's use of the Task Scheduler:\n${output}")
endif()
