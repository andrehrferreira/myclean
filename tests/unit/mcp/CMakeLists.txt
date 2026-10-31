# The MCP server and the health snapshot against a scripted service caller.
myclean_add_test(mcp
  SOURCES
    server_test.cpp
  DEPENDS
    myclean::mcp
    myclean::fakes)
