# core/apps' IAppSystem on Windows: the owner's Appx packages (C++/WinRT PackageManager),
# UserAssist, Prefetch (XPRESS_HUFF decompression), BAM and the system manufacturer. Read-only.
myclean_add_library(platform_apps
  SOURCES
    win32_apps.cpp
    win32_apps.hpp
  DEPENDS
    myclean::apps
    WIL::WIL
    advapi32
    ole32
    shell32
    windowsapp)
# The C++/WinRT headers pull in DirectXMath, whose intrinsics path needs <cpuid.h>, which the
# clang-tidy of Visual Studio does not ship; nothing here uses DirectXMath.
set_source_files_properties(win32_apps.cpp PROPERTIES COMPILE_DEFINITIONS _XM_NO_INTRINSICS_)
