#include "core/mcp/server.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/mcp/client.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::mcp {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;
const std::int64_t noon_seconds =
    std::chrono::duration_cast<std::chrono::seconds>(noon.time_since_epoch()).count();

struct Call {
  std::string method;
  Json params;
};

// metrics.latest as the service answers it, trimmed to what the snapshot reads.
Json latest_reply() {
  return Json{
      {"time", noon_seconds},
      {"cpu", {{"utility", 12.5}, {"processors", Json::array({{{"entity", "0_0"}}})}}},
      {"memory", {{"commit", 40.0}, {"commit_limit", 80.0}}},
      {"disks", Json::array({{{"disk", "0"}, {"queue", 1.5}}, {{"disk", "1"}, {"queue", 0.5}}})},
      {"volumes", Json::array({{{"volume", "D:"}, {"system", false}, {"free_percent", 50.0}},
                               {{"volume", "C:"}, {"system", true}, {"free_percent", 8.0}}})},
      {"unavailable", Json::array()}};
}

// The trust signal of an unsigned image in a user-writable folder.
Json unrecognized_trust() {
  return Json{{"status", "unsigned"},
              {"signer", nullptr},
              {"path", R"(C:\Users\owner\AppData\Local\b\b.exe)"},
              {"path_class", "user_writable"},
              {"unrecognized", true},
              {"reasons", {"not-in-knowledge-base", "unsigned", "user-writable-path"}},
              {"description", nullptr},
              {"category", nullptr}};
}

class ServerTest : public ::testing::Test {
 protected:
  // The service answers every method with {"method": <name>}, metrics.latest with
  // latest_reply(), or fails as `failure_` says.
  Reply answer(std::string_view method, const Json& params) {
    calls_.push_back(Call{.method = std::string(method), .params = params});
    if (failure_) {
      return tl::unexpected(*failure_);
    }
    if (method == "metrics.latest") {
      return latest_reply();
    }
    if (method == "processes.top") {
      return Json{
          {"time", noon_seconds + 1},
          {"filter", {{"cpu_percent", 5.0}, {"memory_percent", 5.0}}},
          {"total_trees", 3},
          {"trees",
           Json::array({{{"pid", 100}, {"image", "a.exe"}, {"trust", nullptr}},
                        {{"pid", 200}, {"image", "b.exe"}, {"trust", unrecognized_trust()}}})}};
    }
    return Json{{"method", method}};
  }

  // The parsed response to one message.
  Json send(const Json& message) {
    const std::optional<std::string> response = server_.handle(message.dump());
    EXPECT_TRUE(response.has_value()) << message.dump();
    return response ? Json::parse(*response) : Json();
  }

  Json call_tool(std::string_view name, Json arguments = Json::object()) {
    return send(Json{{"jsonrpc", "2.0"},
                     {"id", 9},
                     {"method", "tools/call"},
                     {"params", {{"name", name}, {"arguments", std::move(arguments)}}}});
  }

  // The text of a tools/call result, checking its error flag.
  static std::string text(const Json& response, bool error) {
    const Json& result = response.at("result");
    EXPECT_EQ(result.at("isError"), error) << response.dump();
    EXPECT_EQ(result.at("content").size(), 1U);
    EXPECT_EQ(result.at("content")[0].at("type"), "text");
    return result.at("content")[0].at("text").get<std::string>();
  }

  [[nodiscard]] Server& server() { return server_; }
  [[nodiscard]] std::vector<Call>& calls() { return calls_; }
  void fail_calls(CallError error) { failure_ = std::move(error); }

 private:
  fakes::FakeClock clock_{noon};
  std::vector<Call> calls_;
  std::optional<CallError> failure_;
  Server server_{
      [this](std::string_view method, const Json& params) { return answer(method, params); },
      clock_, "1.2.3"};
};

TEST_F(ServerTest, InitializeNegotiatesTheProtocolVersion) {
  const Json known = send(Json{{"jsonrpc", "2.0"},
                               {"id", 1},
                               {"method", "initialize"},
                               {"params",
                                {{"protocolVersion", "2025-03-26"},
                                 {"capabilities", Json::object()},
                                 {"clientInfo", {{"name", "test"}, {"version", "1"}}}}}});
  EXPECT_EQ(known.at("id"), 1);
  EXPECT_EQ(known.at("jsonrpc"), "2.0");
  const Json& result = known.at("result");
  EXPECT_EQ(result.at("protocolVersion"), "2025-03-26");
  EXPECT_EQ(result.at("capabilities"), (Json{{"tools", {{"listChanged", false}}}}));
  EXPECT_EQ(result.at("serverInfo"), (Json{{"name", "myclean"}, {"version", "1.2.3"}}));
  EXPECT_TRUE(result.at("instructions").is_string());

  const Json unknown = send(Json{{"jsonrpc", "2.0"},
                                 {"id", "init"},
                                 {"method", "initialize"},
                                 {"params", {{"protocolVersion", "1999-01-01"}}}});
  EXPECT_EQ(unknown.at("id"), "init");
  EXPECT_EQ(unknown.at("result").at("protocolVersion"), std::string(protocol_versions.front()));
}

TEST_F(ServerTest, NotificationsAndResponsesGetNoAnswer) {
  EXPECT_FALSE(
      server().handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());
  EXPECT_FALSE(server()
                   .handle(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{}})")
                   .has_value());
  EXPECT_FALSE(server().handle(R"({"jsonrpc":"2.0","id":5,"result":{}})").has_value());
  EXPECT_EQ(send(Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}).at("result"),
            Json::object());
}

TEST_F(ServerTest, ListsOnlyReadToolsEachWithAnInputSchema) {
  const Json tools =
      send(Json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}}).at("result").at("tools");
  std::vector<std::string> names;
  for (const Json& tool : tools) {
    names.push_back(tool.at("name").get<std::string>());
    EXPECT_FALSE(tool.at("description").get<std::string>().empty());
    const Json& schema = tool.at("inputSchema");
    EXPECT_EQ(schema.at("type"), "object");
    EXPECT_TRUE(schema.at("properties").is_object());
    EXPECT_EQ(schema.at("additionalProperties"), false);
    EXPECT_EQ(tool.at("annotations").at("readOnlyHint"), true);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"health_snapshot", "metrics_latest", "metrics_history",
                                             "top_processes", "list_volumes", "explain_process",
                                             "disk_health", "list_alerts", "freeze_summary"}));
  EXPECT_EQ(tools[2].at("inputSchema").at("required"), Json::array({"metric"}));
  EXPECT_EQ(tools[5].at("inputSchema").at("required"), Json::array({"pid"}));
  for (const Json& tool : tools) {
    const std::string description = tool.at("description").get<std::string>();
    EXPECT_EQ(description.find("null until"), std::string::npos) << description;
  }
}

TEST_F(ServerTest, ToolsCallTheirReadMethods) {
  EXPECT_EQ(Json::parse(text(call_tool("metrics_latest"), false)), latest_reply());
  EXPECT_EQ(Json::parse(text(call_tool("list_volumes"), false)),
            (Json{{"method", "volumes.list"}}));
  static_cast<void>(text(call_tool("top_processes", {{"sort", "memory"}, {"limit", 3}}), false));
  static_cast<void>(
      text(call_tool("metrics_history", {{"metric", "disk.queue"}, {"entity", "2"}, {"hours", 24}}),
           false));
  static_cast<void>(text(call_tool("metrics_history", {{"metric", "cpu.utility"}}), false));

  EXPECT_EQ(Json::parse(text(call_tool("explain_process", {{"pid", 4321}}), false)),
            (Json{{"method", "processes.explain"}}));
  EXPECT_EQ(Json::parse(text(call_tool("disk_health"), false)), (Json{{"method", "disks.health"}}));
  EXPECT_EQ(Json::parse(text(call_tool("list_alerts"), false)), (Json{{"method", "alerts.list"}}));
  EXPECT_EQ(Json::parse(text(call_tool("freeze_summary"), false)),
            (Json{{"method", "input.summary"}}));

  ASSERT_EQ(calls().size(), 9U);
  EXPECT_EQ(calls()[5].params, (Json{{"pid", 4321}}));
  EXPECT_EQ(calls()[6].params, Json::object());
  EXPECT_EQ(calls()[0].method, "metrics.latest");
  EXPECT_EQ(calls()[1].method, "volumes.list");
  EXPECT_EQ(calls()[2].method, "processes.top");
  EXPECT_EQ(calls()[2].params, (Json{{"sort", "memory"}, {"limit", 3}, {"qualifying", false}}));
  EXPECT_EQ(calls()[3].method, "metrics.query");
  EXPECT_EQ(calls()[3].params, (Json{{"metric", "disk.queue"},
                                     {"entity", "2"},
                                     {"from", noon_seconds - 86'400},
                                     {"max_points", 500}}));
  EXPECT_EQ(calls()[4].params.at("entity"), "system");
  EXPECT_EQ(calls()[4].params.at("from"), noon_seconds - 3600);
}

TEST_F(ServerTest, HealthSnapshotCombinesQualifyingTreesAndSystemSignals) {
  const Json snapshot = Json::parse(text(call_tool("health_snapshot"), false));
  ASSERT_EQ(calls().size(), 2U);
  EXPECT_EQ(calls()[0].method, "processes.top");
  EXPECT_EQ(calls()[0].params, (Json{{"qualifying", true}, {"limit", 100}}));
  EXPECT_EQ(calls()[1].method, "metrics.latest");

  EXPECT_EQ(snapshot.at("trees").size(), 2U);
  EXPECT_EQ(snapshot.at("trees")[1].at("trust"), unrecognized_trust());
  // The unrecognized trees, with their reasons; a tree without a trust signal is not flagged.
  EXPECT_EQ(snapshot.at("unrecognized"),
            Json::array({{{"pid", 200},
                          {"image", "b.exe"},
                          {"path", unrecognized_trust().at("path")},
                          {"reasons", unrecognized_trust().at("reasons")}}}));
  EXPECT_FALSE(snapshot.at("cpu").contains("processors"));
  EXPECT_EQ(snapshot.at("cpu").at("utility"), 12.5);
  EXPECT_EQ(snapshot.at("filter").at("cpu_percent"), 5.0);
  EXPECT_EQ(snapshot.at("total_trees"), 3);
  EXPECT_EQ(snapshot.at("volumes").size(), 2U);
  EXPECT_EQ(snapshot.at("signals"), (Json{{"commit_percent", 50.0},
                                          {"system_volume", "C:"},
                                          {"system_volume_free_percent", 8.0},
                                          {"disk_queue", 2.0}}));
}

TEST_F(ServerTest, ServiceDownIsAToolErrorNamingWhatIsUnavailable) {
  fail_calls(CallError{.reached = false,
                       .message = "the service is not running: nothing listens on its pipe"});
  const std::string message = text(call_tool("health_snapshot"), true);
  EXPECT_NE(message.find("not reachable"), std::string::npos) << message;
  EXPECT_NE(message.find("not running"), std::string::npos) << message;
  EXPECT_NE(message.find("live telemetry"), std::string::npos) << message;
  EXPECT_NE(message.find("disk health"), std::string::npos) << message;

  // The server keeps answering.
  EXPECT_EQ(send(Json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}}).at("result"),
            Json::object());

  fail_calls(CallError{.reached = true, .message = "json-rpc error -32000: no such metric"});
  EXPECT_EQ(text(call_tool("list_volumes"), true),
            "list_volumes failed: json-rpc error -32000: no such metric");
}

TEST_F(ServerTest, BadArgumentsAreToolErrorsAndCallNothing) {
  const std::vector<std::pair<Json, std::string>> cases{
      {Json{{"name", "top_processes"}, {"arguments", {{"pid", 4}}}}, "unknown argument 'pid'"},
      {Json{{"name", "top_processes"}, {"arguments", {{"limit", "3"}}}}, "must be of type integer"},
      {Json{{"name", "top_processes"}, {"arguments", {{"limit", 0}}}}, "out of range"},
      {Json{{"name", "top_processes"}, {"arguments", {{"sort", "disk"}}}}, "must be one of"},
      {Json{{"name", "metrics_history"}, {"arguments", {{"hours", 0}, {"metric", "x"}}}},
       "out of range"},
      {Json{{"name", "metrics_history"}, {"arguments", Json::object()}},
       "missing required argument 'metric'"},
      {Json{{"name", "explain_process"}, {"arguments", Json::object()}},
       "missing required argument 'pid'"},
      {Json{{"name", "explain_process"}, {"arguments", {{"pid", -1}}}}, "out of range"},
      {Json{{"name", "explain_process"}, {"arguments", {{"pid", "C:\\x.exe"}}}},
       "must be of type integer"},
      {Json{{"name", "list_alerts"}, {"arguments", {{"since", 0}}}}, "unknown argument 'since'"},
  };
  for (const auto& [params, expected] : cases) {
    const Json response =
        send(Json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"}, {"params", params}});
    EXPECT_NE(text(response, true).find(expected), std::string::npos) << response.dump();
  }
  EXPECT_TRUE(calls().empty());
}

TEST_F(ServerTest, ProtocolErrorsAreJsonRpcErrors) {
  const auto code = [](const std::optional<std::string>& response) {
    return response ? Json::parse(*response).at("error").at("code").get<int>() : 0;
  };
  const std::optional<std::string> not_json = server().handle("{not json");
  EXPECT_EQ(code(not_json), -32700);
  EXPECT_TRUE(Json::parse(not_json.value_or("{}")).at("id").is_null());
  EXPECT_EQ(code(server().handle(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])")), -32600);
  EXPECT_EQ(code(server().handle(R"({"jsonrpc":"1.0","id":1,"method":"ping"})")), -32600);
  EXPECT_EQ(code(server().handle(R"({"jsonrpc":"2.0","id":1,"method":"resources/list"})")), -32601);
  EXPECT_EQ(code(server().handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":[]})")),
            -32602);
  EXPECT_EQ(code(server().handle(
                R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"arguments":{}}})")),
            -32602);
  // There is no tool that could close a process: asking for one fails without effect.
  const Json kill = call_tool("terminate_process", {{"pid", 1234}});
  EXPECT_EQ(kill.at("error").at("code"), -32602);
  EXPECT_EQ(kill.at("id"), 9);
  EXPECT_TRUE(calls().empty());
}

TEST_F(ServerTest, ServeAnswersOneLinePerRequest) {
  std::istringstream in(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\r\n"
      "\n"
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
  std::ostringstream out;
  server().serve(in, out);
  std::istringstream lines(out.str());
  std::vector<Json> responses;
  for (std::string line; std::getline(lines, line);) {
    EXPECT_EQ(line.find('\r'), std::string::npos);
    responses.push_back(Json::parse(line));
  }
  ASSERT_EQ(responses.size(), 2U);
  EXPECT_EQ(responses[0].at("id"), 1);
  EXPECT_EQ(responses[1].at("id"), 2);
  EXPECT_EQ(responses[1].at("result").at("tools").size(), 9U);
}

}  // namespace
}  // namespace myclean::mcp
