# Win32 adapters of the placement settings (core/placement): user environment variables,
# registry values, tool configuration files, folder copies, system tools (powercfg, vssadmin),
# hibernation, page-file and shadow-storage settings, and the disk seek penalty.
myclean_add_library(platform_settings
  SOURCES
    process_runner.cpp
    process_runner.hpp
    registry.cpp
    registry.hpp
    seek_penalty.cpp
    seek_penalty.hpp
    system_space.cpp
    system_space.hpp
    tree_copier.cpp
    tree_copier.hpp
    user_settings.cpp
    user_settings.hpp
    win32_support.hpp
  DEPENDS
    myclean::placement
    WIL::WIL
    advapi32
    ole32
    oleaut32
    propsys
    user32
    wbemuuid)
