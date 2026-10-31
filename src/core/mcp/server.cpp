#include "core/mcp/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/ipc/telemetry.hpp"
#include "core/mcp/client.hpp"
#include "core/os/clock.hpp"

namespace myclean::mcp {

namespace {

using Json = nlohmann::json;

constexpr int parse_error = -32700;
constexpr int invalid_request = -32600;
constexpr int method_not_found = -32601;
constexpr int invalid_params = -32602;

constexpr double max_history_hours = 2.0 * 365 * 24;  // the hour aggregates' retention

std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string error_response(const Json& id, int code, const std::string& message) {
  return dump(
      Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

Json object_schema(Json properties = Json::object(), Json required = Json::array()) {
  return Json{{"type", "object"},
              {"properties", std::move(properties)},
              {"required", std::move(required)},
              {"additionalProperties", false}};
}

Json tool(std::string_view name, std::string_view description, Json schema) {
  return Json{{"name", name},
              {"description", description},
              {"inputSchema", std::move(schema)},
              {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}};
}

const Json& tool_list() {
  static const Json list = Json::array({
      tool("health_snapshot",
           "Current health of this PC in one call: every process tree using more than 5 % of "
           "all logical processors or whose private working set exceeds 5 % of physical memory "
           "(each expandable to its member processes), CPU, memory, per-disk and per-volume "
           "figures with days until full, GPU, and signals (commit charge %, system volume free "
           "%, total disk queue). Each tree's 'trust' is null until MyClean's trust signal is "
           "available. Reads only; changes nothing.",
           object_schema()),
      tool("metrics_latest",
           "The newest CPU, memory, process-count, per-disk, GPU and per-volume measurements. "
           "Bytes are bytes, rates are per second, percentages are 0-100. Reads only.",
           object_schema()),
      tool("metrics_history",
           "History of one metric over the last `hours`, at the finest resolution kept for that "
           "range (1 s for the last hour, 1 min for 30 days, 1 h beyond). Metrics: cpu.utility, "
           "cpu.frequency, cpu.dpc, cpu.interrupt, cpu.queue, memory.available, memory.commit, "
           "memory.commit_limit, memory.standby, memory.modified, memory.compressed, "
           "memory.hard_faults, disk.read_bytes, disk.write_bytes, disk.iops, "
           "disk.read_latency, disk.write_latency, disk.queue, disk.idle, volume.size, "
           "volume.free, gpu.utilization, gpu.dedicated, gpu.shared, process.count. Reads only.",
           object_schema(
               {{"metric",
                 {{"type", "string"}, {"description", "Metric name, such as cpu.utility"}}},
                {"entity",
                 {{"type", "string"},
                  {"description",
                   "What the metric is about: 'system' (default), a disk number ('0'), a volume "
                   "entity from list_volumes ('C'), a logical processor ('0_17') or a GPU "
                   "adapter entity from metrics_latest"}}},
                {"hours",
                 {{"type", "number"},
                  {"exclusiveMinimum", 0},
                  {"maximum", max_history_hours},
                  {"description", "How far back from now, in hours (default 1)"}}},
                {"max_points",
                 {{"type", "integer"},
                  {"minimum", 1},
                  {"maximum", 10000},
                  {"description", "Merge points so that at most this many return (default 500)"}}}},
               Json::array({"metric"}))),
      tool("top_processes",
           "Process trees (a root process with its descendants) ordered by CPU or by memory, "
           "with totals and members. Each tree's 'trust' is null until MyClean's trust signal "
           "is available. Reads only; changes nothing.",
           object_schema(
               {{"sort", {{"type", "string"}, {"enum", {"cpu", "memory"}}, {"default", "cpu"}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", ipc::max_top_trees}}},
                {"qualifying_only",
                 {{"type", "boolean"},
                  {"description",
                   "Only trees above 5 % CPU or 5 % of physical memory (default false)"}}}})),
      tool("list_volumes",
           "Every mounted volume: size, free space, physical disks, free-space trend and days "
           "until full. Reads only.",
           object_schema()),
  });
  return list;
}

bool has_type(const Json& value, std::string_view type) {
  if (type == "string") {
    return value.is_string();
  }
  if (type == "integer") {
    return value.is_number_integer();
  }
  if (type == "number") {
    return value.is_number();
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  return false;
}

// Why `arguments` do not fit `schema` (the subset tool_list() uses), or an empty string.
std::string check_arguments(const Json& schema, const Json& arguments) {
  const Json& properties = schema.at("properties");
  for (const auto& [name, value] : arguments.items()) {
    const auto property = properties.find(name);
    if (property == properties.end()) {
      return "unknown argument '" + name + "'";
    }
    const auto type = property->at("type").get<std::string>();
    if (!has_type(value, type)) {
      return std::format("argument '{}' must be of type {}", name, type);
    }
    if (const auto allowed = property->find("enum");
        allowed != property->end() && std::ranges::find(*allowed, value) == allowed->end()) {
      return "argument '" + name + "' must be one of " + dump(*allowed);
    }
    if (value.is_number()) {
      const double number = value.get<double>();
      if ((property->contains("minimum") && number < property->at("minimum").get<double>()) ||
          (property->contains("exclusiveMinimum") &&
           number <= property->at("exclusiveMinimum").get<double>()) ||
          (property->contains("maximum") && number > property->at("maximum").get<double>())) {
        return "argument '" + name + "' is out of range";
      }
    }
  }
  for (const Json& name : schema.at("required")) {
    if (!arguments.contains(name.get<std::string>())) {
      return "missing required argument '" + name.get<std::string>() + "'";
    }
  }
  return {};
}

Json tool_result(std::string text, bool error) {
  return Json{{"content", Json::array({Json{{"type", "text"}, {"text", std::move(text)}}})},
              {"isError", error}};
}

}  // namespace

Server::Server(Caller call, const os::IClock& clock, std::string version)
    : call_(std::move(call)), clock_(&clock), version_(std::move(version)) {}

Json Server::tools() {
  return Json{{"tools", tool_list()}};
}

std::optional<std::string> Server::handle(std::string_view message) {
  const Json request = Json::parse(message, nullptr, false);
  if (request.is_discarded()) {
    return error_response(nullptr, parse_error, "parse error: the message is not JSON");
  }
  if (!request.is_object()) {
    return error_response(nullptr, invalid_request, "the message is not a JSON-RPC object");
  }
  const auto method = request.find("method");
  const auto id = request.find("id");
  if (method == request.end()) {
    return std::nullopt;  // a response; this server sends no requests
  }
  if (id == request.end()) {
    return std::nullopt;  // a notification, such as notifications/initialized
  }
  const auto version = request.find("jsonrpc");
  if (!(id->is_string() || id->is_number_integer()) || !method->is_string() ||
      version == request.end() || *version != "2.0") {
    return error_response(id->is_string() || id->is_number_integer() ? *id : Json(nullptr),
                          invalid_request, "not a JSON-RPC 2.0 request");
  }
  const Json params = request.value("params", Json::object());
  if (!params.is_object()) {
    return error_response(*id, invalid_params, "params must be an object");
  }

  const auto& name = method->get_ref<const std::string&>();
  Json result;
  if (name == "initialize") {
    result = initialize(params);
  } else if (name == "ping") {
    result = Json::object();
  } else if (name == "tools/list") {
    result = tools();
  } else if (name == "tools/call") {
    tl::expected<Json, std::string> called = call_tool(params);
    if (!called) {
      return error_response(*id, invalid_params, called.error());
    }
    result = std::move(*called);
  } else {
    return error_response(*id, method_not_found, "method not found: " + name);
  }
  return dump(Json{{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(result)}});
}

void Server::serve(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.find_first_not_of(" \t") == std::string::npos) {
      continue;
    }
    if (const std::optional<std::string> response = handle(line)) {
      out << *response << '\n' << std::flush;
    }
  }
}

Json Server::initialize(const Json& params) const {
  const auto requested = params.find("protocolVersion");
  std::string_view version = protocol_versions[0];
  if (requested != params.end() && requested->is_string() &&
      std::ranges::find(protocol_versions, requested->get<std::string>()) !=
          protocol_versions.end()) {
    version = requested->get_ref<const std::string&>();
  }
  return Json{
      {"protocolVersion", version},
      {"capabilities", {{"tools", {{"listChanged", false}}}}},
      {"serverInfo", {{"name", "myclean"}, {"version", version_}}},
      {"instructions",
       "Read-only view of this PC from the MyClean service: live CPU, memory, disk, GPU, volume "
       "and process-tree telemetry and its history. No tool changes anything. Process trees "
       "carry a 'trust' field that is null until MyClean's trust signal is available."}};
}

tl::expected<Json, std::string> Server::call_tool(const Json& params) const {
  const auto name = params.find("name");
  if (name == params.end() || !name->is_string()) {
    return tl::unexpected<std::string>("tools/call needs the tool name");
  }
  const Json& list = tool_list();
  const auto tool =
      std::ranges::find_if(list, [&](const Json& entry) { return entry.at("name") == *name; });
  if (tool == list.end()) {
    return tl::unexpected("unknown tool: " + name->get<std::string>());
  }
  const Json arguments = params.value("arguments", Json::object());
  if (!arguments.is_object()) {
    return tl::unexpected<std::string>("tools/call arguments must be an object");
  }
  if (const std::string problem = check_arguments(tool->at("inputSchema"), arguments);
      !problem.empty()) {
    return tool_result(name->get<std::string>() + ": " + problem, true);
  }

  const auto& tool_name = name->get_ref<const std::string&>();
  Reply reply;
  if (tool_name == "health_snapshot") {
    reply = health_snapshot(call_);
  } else if (tool_name == "metrics_latest") {
    reply = call_(ipc::metrics_latest_method, Json::object());
  } else if (tool_name == "metrics_history") {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(clock_->now().time_since_epoch()).count();
    const auto span = static_cast<std::int64_t>(arguments.value("hours", 1.0) * 3600.0);
    reply =
        call_(ipc::metrics_query_method, Json{{"metric", arguments.at("metric")},
                                              {"entity", arguments.value("entity", "system")},
                                              {"from", now - std::max<std::int64_t>(span, 1)},
                                              {"max_points", arguments.value("max_points", 500)}});
  } else if (tool_name == "top_processes") {
    reply = call_(ipc::processes_top_method,
                  Json{{"sort", arguments.value("sort", "cpu")},
                       {"limit", arguments.value("limit", 10)},
                       {"qualifying", arguments.value("qualifying_only", false)}});
  } else {
    reply = call_(ipc::volumes_list_method, Json::object());
  }
  if (!reply) {
    return tool_result(reply.error().reached ? tool_name + " failed: " + reply.error().message
                                             : unavailable_text(reply.error().message),
                       true);
  }
  return tool_result(dump(*reply), false);
}

}  // namespace myclean::mcp
