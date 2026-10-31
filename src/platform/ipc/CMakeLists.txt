# The service pipe over Win32: \\.\pipe\myclean server and client, and the SID and security
# descriptor helpers that build its DACL.
myclean_add_library(platform_ipc
  SOURCES
    pipe_client.cpp
    pipe_client.hpp
    pipe_io.cpp
    pipe_io.hpp
    pipe_server.cpp
    pipe_server.hpp
    security.cpp
    security.hpp
  DEPENDS
    myclean::ipc
    WIL::WIL
    advapi32)
