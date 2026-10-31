# The read side for AI and scripts: the health snapshot composed from the service's read methods
# and the MCP server (JSON-RPC 2.0 over stdio) with read-only tools. The pipe itself is the
# caller's business (src/cli), so everything here runs against any Caller.
myclean_add_library(mcp
  SOURCES
    client.cpp
    client.hpp
    server.cpp
    server.hpp
  DEPENDS
    myclean::ipc
    myclean::os
    nlohmann_json::nlohmann_json)
