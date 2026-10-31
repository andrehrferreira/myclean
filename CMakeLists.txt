cmake_minimum_required(VERSION 3.25)

project(myclean VERSION 0.1.0 DESCRIPTION "MyClean PC monitor, cleaner and resource governor" LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "MyClean targets Windows and builds with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MYCLEAN_ASAN "Build with AddressSanitizer (set by the debug preset)" OFF)

# Debug information goes into the objects (/Z7); the linker writes one PDB per binary, in every
# configuration, so Release ships PDBs as well (deploy contract C2).
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
add_link_options(/DEBUG $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

# Every first-party translation unit: warnings are errors. Third-party headers come in through
# vcpkg's imported targets as external includes and are compiled with /external:W0.
add_compile_options(/W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MYCLEAN_ASAN)
  # /RTC and incremental linking are incompatible with AddressSanitizer.
  string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  add_compile_options(/fsanitize=address)
  # vcpkg libraries are built without ASan; the STL's container annotations must match across
  # everything linked (LNK2038), so they are off. Heap, stack and use-after-free checks remain.
  add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
  add_link_options(/INCREMENTAL:NO)
  # ASan binaries load the runtime DLL from the toolset's bin directory, which is only on PATH in
  # a developer shell; myclean_add_test copies it next to each test executable instead.
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _myclean_msvc_bin)
  find_file(MYCLEAN_ASAN_RUNTIME clang_rt.asan_dynamic-x86_64.dll
    PATHS "${_myclean_msvc_bin}" NO_DEFAULT_PATH REQUIRED)
endif()

# All third-party packages are found here, once, so that every module and test can link them.
find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

# The toml++ 3.4.0 port ships only a pkg-config file; this is the target that file describes.
set(_myclean_vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(MYCLEAN_TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp
  PATHS "${_myclean_vcpkg_prefix}/include" NO_DEFAULT_PATH REQUIRED)
find_library(MYCLEAN_TOMLPLUSPLUS_LIBRARY_RELEASE tomlplusplus
  PATHS "${_myclean_vcpkg_prefix}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(MYCLEAN_TOMLPLUSPLUS_LIBRARY_DEBUG tomlplusplus
  PATHS "${_myclean_vcpkg_prefix}/debug/lib" NO_DEFAULT_PATH REQUIRED)
add_library(tomlplusplus::tomlplusplus STATIC IMPORTED)
set_target_properties(tomlplusplus::tomlplusplus PROPERTIES
  IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
  IMPORTED_LOCATION_RELEASE "${MYCLEAN_TOMLPLUSPLUS_LIBRARY_RELEASE}"
  IMPORTED_LOCATION_DEBUG "${MYCLEAN_TOMLPLUSPLUS_LIBRARY_DEBUG}"
  INTERFACE_INCLUDE_DIRECTORIES "${MYCLEAN_TOMLPLUSPLUS_INCLUDE_DIR}"
  INTERFACE_COMPILE_DEFINITIONS "TOML_HEADER_ONLY=0")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(MyClean)

enable_testing()
include(GoogleTest)

add_subdirectory(src)
add_subdirectory(tests)

# Data files ship next to bin/ as data/ (deploy contract C2).
install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/" DESTINATION data OPTIONAL)

include(Lint)
