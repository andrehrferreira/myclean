# Helpers shared by every module. See docs/development.md, "Adding a module".

# Adds every immediate subdirectory of the calling directory that contains a CMakeLists.txt,
# in name order. A new module is therefore a new folder; no parent CMakeLists.txt changes.
function(myclean_add_subdirectories)
  file(GLOB children LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
  list(SORT children)
  foreach(child IN LISTS children)
    if(IS_DIRECTORY "${child}" AND EXISTS "${child}/CMakeLists.txt")
      add_subdirectory("${child}")
    endif()
  endforeach()
endfunction()

# Records first-party .cpp files for the lint target (see cmake/Lint.cmake).
function(_myclean_register_lint_sources)
  foreach(source IN LISTS ARGN)
    if(source MATCHES "\\.cpp$")
      cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
      set_property(GLOBAL APPEND PROPERTY MYCLEAN_LINT_SOURCES "${source}")
    endif()
  endforeach()
endfunction()

# myclean_add_library(<name> [SOURCES <file>...] [DEPENDS <target>...] [INCLUDE_ROOT <dir>])
#
# Defines target myclean_<name> with alias myclean::<name>. It is a static library when SOURCES
# contains a .cpp file and an interface (header-only) library otherwise. Headers are included
# relative to INCLUDE_ROOT (default: <repo>/src), e.g. #include "core/model/plan.hpp".
# DEPENDS are linked publicly.
function(myclean_add_library name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "INCLUDE_ROOT" "SOURCES;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "myclean_add_library(${name}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT arg_INCLUDE_ROOT)
    set(arg_INCLUDE_ROOT "${PROJECT_SOURCE_DIR}/src")
  endif()

  set(target myclean_${name})
  set(compiled ${arg_SOURCES})
  list(FILTER compiled INCLUDE REGEX "\\.cpp$")
  if(compiled)
    add_library(${target} STATIC ${arg_SOURCES})
    target_include_directories(${target} PUBLIC "${arg_INCLUDE_ROOT}")
    target_link_libraries(${target} PUBLIC ${arg_DEPENDS})
  else()
    add_library(${target} INTERFACE ${arg_SOURCES})
    target_include_directories(${target} INTERFACE "${arg_INCLUDE_ROOT}")
    target_link_libraries(${target} INTERFACE ${arg_DEPENDS})
  endif()
  add_library(myclean::${name} ALIAS ${target})
  _myclean_register_lint_sources(${arg_SOURCES})
endfunction()

# myclean_add_test(<name> SOURCES <file>... [DEPENDS <target>...])
#
# Defines the GoogleTest executable myclean_<name>_tests, linked with gtest_main and DEPENDS,
# and registers each test case with CTest under the label <name>.
function(myclean_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "myclean_add_test(${name}): needs SOURCES and only SOURCES/DEPENDS")
  endif()

  set(target myclean_${name}_tests)
  add_executable(${target} ${arg_SOURCES})
  target_link_libraries(${target} PRIVATE ${arg_DEPENDS} GTest::gtest_main)
  if(MYCLEAN_ASAN)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MYCLEAN_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endif()
  # Tests are discovered when ctest runs, so a build never executes test binaries.
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS ${name})
  _myclean_register_lint_sources(${arg_SOURCES})
endfunction()

# myclean_install_executable(<target>)
#
# Installs an executable and its PDB into bin/ (deploy contract C2). Third-party libraries are
# linked statically (triplet x64-windows-static-md), so there are no third-party DLLs to copy.
function(myclean_install_executable target)
  install(TARGETS ${target} RUNTIME DESTINATION bin)
  install(FILES "$<TARGET_PDB_FILE:${target}>" DESTINATION bin OPTIONAL)
endfunction()
