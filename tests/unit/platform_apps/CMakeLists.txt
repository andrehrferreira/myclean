# The apps reader against this machine as the test's own user, read paths only: its Appx
# packages, UserAssist, the maker, and Prefetch and BAM (which need administrator rights). Nothing
# is started, removed or changed.
myclean_add_test(platform_apps
  SOURCES
    win32_apps_test.cpp
  DEPENDS
    myclean::platform_apps
    myclean::platform_ipc)
