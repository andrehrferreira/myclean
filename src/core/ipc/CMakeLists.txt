# The service protocol: length-prefixed frames, the JSON-RPC 2.0 dispatcher with typed
# parameters, client-side request/response helpers, and the replies shared by service and clients.
myclean_add_library(ipc
  SOURCES
    frame.cpp
    frame.hpp
    json_rpc.cpp
    json_rpc.hpp
    status.cpp
    status.hpp
    telemetry.cpp
    telemetry.hpp
  DEPENDS
    myclean::model
    nlohmann_json::nlohmann_json)
