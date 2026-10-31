# Quality targets: check-core-includes (part of every build), lint, format-check and format.
# Included at the end of the root CMakeLists.txt, after every module registered its sources.

# Prefer the clang tools bundled with the Visual Studio instance that provides the compiler, so
# that every machine with the same Visual Studio formats and lints identically.
cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _myclean_cl_dir)
set(_myclean_llvm_hints "${_myclean_cl_dir}/../../../../../Llvm/x64/bin")
if(DEFINED ENV{VCINSTALLDIR})
  list(APPEND _myclean_llvm_hints "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin")
endif()
find_program(MYCLEAN_CLANG_TIDY NAMES clang-tidy HINTS ${_myclean_llvm_hints})
find_program(MYCLEAN_CLANG_FORMAT NAMES clang-format HINTS ${_myclean_llvm_hints})

# src/core must not reach the OS directly: no Win32 header may be included there.
add_custom_target(check-core-includes ALL
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
          -P "${PROJECT_SOURCE_DIR}/cmake/scripts/check-core-includes.cmake"
  COMMENT "Checking that src/core includes no platform headers"
  VERBATIM)

# A target that only reports a missing tool, so configure still works without clang tools.
function(_myclean_missing_tool_target target tool)
  add_custom_target(${target}
    COMMAND "${CMAKE_COMMAND}" -E echo "${target}: ${tool} not found. Install the 'C++ Clang tools for Windows' Visual Studio component."
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM)
endfunction()

if(MYCLEAN_CLANG_FORMAT)
  foreach(mode IN ITEMS check fix)
    if(mode STREQUAL "check")
      set(target format-check)
    else()
      set(target format)
    endif()
    add_custom_target(${target}
      COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
              "-DCLANG_FORMAT=${MYCLEAN_CLANG_FORMAT}" "-DMODE=${mode}"
              -P "${PROJECT_SOURCE_DIR}/cmake/scripts/format.cmake"
      VERBATIM)
  endforeach()
else()
  _myclean_missing_tool_target(format-check clang-format)
  _myclean_missing_tool_target(format clang-format)
endif()

# New clang-tidy majors add checks that the enabled groups pick up (e.g. portability-* and
# cppcoreguidelines-* in 20+), so every machine and CI must lint with the same major as the
# Visual Studio 2022 bundle. CI installs that version and passes it as MYCLEAN_CLANG_TIDY.
set(_myclean_clang_tidy_major 19)
if(MYCLEAN_CLANG_TIDY)
  execute_process(COMMAND "${MYCLEAN_CLANG_TIDY}" --version OUTPUT_VARIABLE _myclean_tidy_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _myclean_tidy_version MATCHES "version ${_myclean_clang_tidy_major}\\.")
    message(WARNING "lint disabled: clang-tidy ${_myclean_clang_tidy_major}.x is required, "
      "found ${MYCLEAN_CLANG_TIDY}: ${_myclean_tidy_version}")
    set(_myclean_tidy_usable FALSE)
  else()
    set(_myclean_tidy_usable TRUE)
  endif()
endif()

if(MYCLEAN_CLANG_TIDY AND _myclean_tidy_usable)
  # One clang-tidy run per source file, so Ninja runs them in parallel and re-runs only what
  # changed. Any first-party header change re-lints every file (clang-tidy writes no depfile).
  file(GLOB_RECURSE _myclean_headers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.hpp" "${PROJECT_SOURCE_DIR}/tests/*.hpp")
  get_property(_myclean_lint_sources GLOBAL PROPERTY MYCLEAN_LINT_SOURCES)
  list(REMOVE_DUPLICATES _myclean_lint_sources)
  set(_myclean_stamps)
  foreach(source IN LISTS _myclean_lint_sources)
    cmake_path(RELATIVE_PATH source BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE relative)
    set(stamp "${CMAKE_BINARY_DIR}/lint/${relative}.stamp")
    add_custom_command(OUTPUT "${stamp}"
      # clang-cl refuses /fsanitize=address with the debug runtime (/MDd), and crashes on
      # -fno-sanitize, so the analysis uses the release runtime instead.
      COMMAND "${MYCLEAN_CLANG_TIDY}" --quiet -p "${CMAKE_BINARY_DIR}" --extra-arg=/MD "${source}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/.clang-tidy" ${_myclean_headers}
      COMMENT "clang-tidy ${relative}"
      VERBATIM)
    list(APPEND _myclean_stamps "${stamp}")
  endforeach()
  add_custom_target(lint DEPENDS ${_myclean_stamps})
  add_dependencies(lint check-core-includes)
else()
  _myclean_missing_tool_target(lint clang-tidy)
endif()
