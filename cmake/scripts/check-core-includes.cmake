# Fails when a file under src/core includes anything other than a C++ standard header, another
# core header ("core/...") or an approved portable third-party library. This keeps <windows.h>
# and every other Win32 header out of core: core reaches the OS only through core/os interfaces.
#
# Usage: cmake -DSOURCE_DIR=<repo> -P check-core-includes.cmake

# A script sets no policies of its own: without this, if(IN_LIST) is not an operator.
cmake_minimum_required(VERSION 3.25)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is not set")
endif()

set(standard_headers
  algorithm any array atomic barrier bit bitset charconv chrono compare complex concepts
  condition_variable coroutine deque exception execution expected filesystem format forward_list
  fstream functional future initializer_list iomanip ios iosfwd iostream istream iterator latch
  limits list locale map memory memory_resource mutex new numbers numeric optional ostream queue
  random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location span
  sstream stack stdexcept stop_token streambuf string string_view syncstream system_error thread
  tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector
  version
  cassert cctype cerrno cfenv cfloat cinttypes climits clocale cmath csetjmp csignal cstdarg
  cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype)

# Portable third-party libraries from vcpkg.json that core may use (header path prefixes).
set(third_party_prefixes "tl/" "sqlite3.h" "toml++/" "nlohmann/" "spdlog/" "fmt/")

file(GLOB_RECURSE files "${SOURCE_DIR}/src/core/*.cpp" "${SOURCE_DIR}/src/core/*.hpp"
  "${SOURCE_DIR}/src/core/*.h" "${SOURCE_DIR}/src/core/*.ipp")
list(SORT files)

set(violations "")
foreach(file IN LISTS files)
  file(STRINGS "${file}" lines)
  set(line_number 0)
  foreach(line IN LISTS lines)
    math(EXPR line_number "${line_number} + 1")
    if(NOT line MATCHES "^[ \t]*#[ \t]*include[ \t]*([<\"])([^>\"]+)[>\"]")
      continue()
    endif()
    set(delimiter "${CMAKE_MATCH_1}")
    set(header "${CMAKE_MATCH_2}")
    set(allowed FALSE)
    if(delimiter STREQUAL "\"")
      if(header MATCHES "^core/")
        set(allowed TRUE)
      endif()
    elseif(header IN_LIST standard_headers)
      set(allowed TRUE)
    else()
      foreach(prefix IN LISTS third_party_prefixes)
        string(FIND "${header}" "${prefix}" position)
        if(position EQUAL 0)
          set(allowed TRUE)
        endif()
      endforeach()
    endif()
    if(NOT allowed)
      cmake_path(RELATIVE_PATH file BASE_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE relative)
      if(delimiter STREQUAL "\"")
        set(spelled "\"${header}\"")
      else()
        set(spelled "<${header}>")
      endif()
      string(APPEND violations "${relative}:${line_number}: include ${spelled} is not allowed in src/core\n")
    endif()
  endforeach()
endforeach()

if(violations)
  message(FATAL_ERROR
    "${violations}"
    "src/core may include only C++ standard headers, \"core/...\" headers and the portable "
    "libraries listed in cmake/scripts/check-core-includes.cmake. Reach the OS through the "
    "interfaces in src/core/os and implement them under src/platform.")
endif()
