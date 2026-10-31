# Cleaning through Windows and the tools themselves (C3): disk-cleanup handlers, native prune
# commands under the user token and DISM component cleanup, as Actions over the runner and
# handler-host interfaces that src/platform/cleanup implements.
myclean_add_library(cleanup
  SOURCES
    command_runner.hpp
    dism_cleanup.cpp
    dism_cleanup.hpp
    handler_cleanup.cpp
    handler_cleanup.hpp
    handler_host.hpp
    native_command.cpp
    native_command.hpp
    support.cpp
    support.hpp
  DEPENDS
    myclean::actions
    myclean::model
    myclean::os
    myclean::safety)
