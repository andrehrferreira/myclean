# Win32 implementations of the space interfaces: directory listing for the scanner, the
# Zone.Identifier reader, the content hasher and the installer inspector. All read-only.
myclean_add_library(platform_scan
  SOURCES
    directory_reader.cpp
    directory_reader.hpp
    file_content.cpp
    file_content.hpp
    installer_inspector.cpp
    installer_inspector.hpp
    scan_support.cpp
    scan_support.hpp
  DEPENDS
    myclean::space
    WIL::WIL
    advapi32
    bcrypt
    msi
    ole32
    shell32
    version)
