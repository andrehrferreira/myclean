# `myclean scan-all` argument handling through cli::run. Only refused command lines: nothing here
# scans a drive.
myclean_add_test(scan_all_cli
  SOURCES
    scan_all_cli_test.cpp
  DEPENDS
    myclean::cli)
