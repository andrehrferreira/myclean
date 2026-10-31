# Toolchain used by every preset: locates vcpkg and chains to its toolchain file.
# Uses $VCPKG_ROOT when set; otherwise falls back to C:/vcpkg (see docs/development.md).

if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _myclean_vcpkg_root)
else()
  set(_myclean_vcpkg_root "C:/vcpkg")
endif()

set(_myclean_vcpkg_toolchain "${_myclean_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
if(NOT EXISTS "${_myclean_vcpkg_toolchain}")
  message(FATAL_ERROR
    "vcpkg not found at '${_myclean_vcpkg_root}'. Set VCPKG_ROOT to a vcpkg clone "
    "(bootstrapped with bootstrap-vcpkg.bat) or clone vcpkg to C:/vcpkg.")
endif()

include("${_myclean_vcpkg_toolchain}")
