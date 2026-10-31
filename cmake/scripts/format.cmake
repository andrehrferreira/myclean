# Runs clang-format over every first-party C++ file under src/ and tests/.
# MODE=check fails on any formatting difference; MODE=fix rewrites the files in place.
#
# Usage: cmake -DSOURCE_DIR=<repo> -DCLANG_FORMAT=<exe> -DMODE=check|fix -P format.cmake

foreach(variable IN ITEMS SOURCE_DIR CLANG_FORMAT MODE)
  if(NOT ${variable})
    message(FATAL_ERROR "${variable} is not set")
  endif()
endforeach()

if(MODE STREQUAL "check")
  set(mode_args --dry-run --Werror)
elseif(MODE STREQUAL "fix")
  set(mode_args -i)
else()
  message(FATAL_ERROR "MODE must be check or fix, not '${MODE}'")
endif()

file(GLOB_RECURSE files RELATIVE "${SOURCE_DIR}"
  "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.hpp"
  "${SOURCE_DIR}/tests/*.cpp" "${SOURCE_DIR}/tests/*.hpp")
list(SORT files)
list(LENGTH files count)

# Batches keep each command line well under the Windows limit.
set(batch_size 50)
set(failed FALSE)
set(start 0)
while(start LESS count)
  list(SUBLIST files ${start} ${batch_size} batch)
  execute_process(COMMAND "${CLANG_FORMAT}" --style=file ${mode_args} ${batch}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    set(failed TRUE)
  endif()
  math(EXPR start "${start} + ${batch_size}")
endwhile()

if(failed)
  message(FATAL_ERROR "clang-format reported differences; run: cmake --build --preset <name> --target format")
endif()
message(STATUS "clang-format ${MODE}: ${count} files")
