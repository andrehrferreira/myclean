# Adapter tests against the real file system inside a unique folder under %TEMP%\myclean-it, plus
# read-only checks of system files, the installed-programs list and a capped smoke scan of
# %WINDIR%\System32\drivers.
myclean_add_test(platform_scan
  SOURCES
    directory_reader_test.cpp
    file_content_test.cpp
    installer_inspector_test.cpp
    sandbox.cpp
    sandbox.hpp
  DEPENDS
    myclean::platform_scan
    wofutil)
