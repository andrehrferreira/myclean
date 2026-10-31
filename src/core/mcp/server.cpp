#include "core/mcp/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/ipc/alerts.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
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

// A tool that creates a plan waiting for the owner's approval: it changes no file or setting.
Json proposal_tool(std::string_view name, std::string_view description, Json schema) {
  return Json{{"name", name},
              {"description", description},
              {"inputSchema", std::move(schema)},
              {"annotations",
               {{"readOnlyHint", false},
                {"destructiveHint", false},
                {"idempotentHint", false},
                {"openWorldHint", false}}}};
}

const Json& refresh_property() {
  static const Json property{
      {"type", "boolean"},
      {"description", "Start a new analysis even when the last result is recent (default false)"}};
  return property;
}

constexpr std::string_view space_note =
    " The analysis runs in the background: while 'state' is 'scanning', call again in a minute. "
    "Every file entry has an 'id' to pass to propose_cleanup. Reads only; changes nothing.";

const Json& tool_list() {
  static const Json list = Json::array({
      tool("health_snapshot",
           "Current health of this PC in one call: every process tree using more than 5 % of "
           "all logical processors or whose private working set exceeds 5 % of physical memory "
           "(each expandable to its member processes), CPU, memory, per-disk and per-volume "
           "figures with days until full, GPU, and signals (commit charge %, system volume free "
           "%, total disk queue). Each tree carries 'trust' for its root process: signature "
           "status (signed, unsigned, invalid), signer, path, path class (protected or "
           "user_writable), the knowledge-base description and category (null: unknown), and "
           "'unrecognized' with its reasons; 'unrecognized' lists those trees. Unrecognized is a "
           "fact, not a verdict: never call a process dangerous or malicious. 'trust' is null "
           "when the image cannot be read (System, protected processes). Reads only; changes "
           "nothing.",
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
           "with totals, members and the trust signal of the root process (as in "
           "health_snapshot). Reads only; changes nothing.",
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
      tool("explain_process",
           "One running process explained: image, path, its process tree and the tree's "
           "members, current CPU and memory use, signer and trust signal with reasons, and the "
           "knowledge-base entry (plain description, category, whether it is needed at startup "
           "and safe to remove; null when the process is not in the knowledge base). Fails when "
           "no process has the PID. Reads only; changes nothing.",
           object_schema({{"pid",
                           {{"type", "integer"},
                            {"minimum", 0},
                            {"maximum", 4294967295.0},
                            {"description", "Process id, from health_snapshot or top_processes"}}}},
                         Json::array({"pid"}))),
      tool("disk_health",
           "Health of every physical disk: model, bus, volumes, status (good, warning, failing, "
           "unknown) with reasons, temperature against the model's rated limit, wear, spare, "
           "media errors, reallocated and pending sectors, power-on hours, data written and the "
           "write-endurance projection. A drive whose own health data cannot be read (such as "
           "one behind a USB bridge) says why. Reads only.",
           object_schema()),
      tool("list_alerts",
           "Alerts MyClean has raised (drive almost full or filling up, disk health changes, "
           "drive temperature), newest first: rule, entity, title, severity, first and last "
           "time, count, whether active or snoozed, and detail. Reads only; snoozing and "
           "dismissing are left to the owner.",
           object_schema()),
      tool("freeze_summary",
           "Copy/paste freezes over 250 ms recorded by the input watchdog, grouped by "
           "mechanism (delayed rendering, clipboard held open, viewer chain, slow keyboard hook, "
           "heavy payload, unexplained) and by the process involved, with counts, latest time "
           "and each incident's evidence. Reads only.",
           object_schema()),
      tool("storage_top",
           std::string("The largest files and folders of a drive or of the owner's Downloads or "
                       "profile folder, by size on disk.") +
               std::string(space_note),
           object_schema({{"path",
                           {{"type", "string"},
                            {"description",
                             "A drive letter such as 'C' (default), or 'downloads' or 'profile'"}}},
                          {"n",
                           {{"type", "integer"},
                            {"minimum", 1},
                            {"maximum", ipc::max_space_top},
                            {"description", "How many files and folders (default 20)"}}},
                          {"refresh", refresh_property()}})),
      tool("find_old_downloads",
           std::string("Files in the owner's Downloads folder created and modified more than "
                       "90 days ago, with size, age and origin (the URL of the download, when "
                       "Windows recorded it).") +
               std::string(space_note),
           object_schema({{"refresh", refresh_property()}})),
      tool("find_duplicates",
           std::string("Exact duplicate files (equal content hash) in the owner's Desktop, "
                       "Documents, Downloads, Pictures, Videos and Music, grouped, with the space "
                       "each group could free.") +
               std::string(space_note),
           object_schema({{"refresh", refresh_property()}})),
      tool("find_used_installers",
           std::string("Installers (.msi, .exe) in the owner's Downloads whose product is "
                       "installed already.") +
               std::string(space_note),
           object_schema({{"refresh", refresh_property()}})),
      tool("list_plans",
           "MyClean's plans, newest first: id, title, kind (cleanup, handler, native, relocate, "
           "system_setting, quarantine_restore), tier, scope, state, proposer, items, bytes and "
           "whether a restart is needed. Plans in 'awaiting_approval' wait for the owner in the "
           "MyClean tray. Reads only.",
           object_schema({{"state",
                           {{"type", "string"},
                            {"enum",
                             {"proposed", "auto", "awaiting_approval", "approved", "applying",
                              "applied", "verified", "rolled_back", "failed", "rejected"}},
                            {"description", "Only plans in this state"}}}})),
      tool("get_plan",
           "One plan with its items (path, bytes, rule, reason, approval), the exact commands "
           "it runs, how it is undone, warnings, bytes freed once applied and why it failed. "
           "Reads only.",
           object_schema({{"id", {{"type", "integer"}, {"minimum", 1}}}}, Json::array({"id"}))),
      proposal_tool(
          "propose_cleanup",
          "Proposes moving files found by storage_top, find_old_downloads, find_duplicates or "
          "find_used_installers to MyClean's quarantine (restorable for 7 days). It only creates "
          "a plan in state 'awaiting_approval' and notifies the owner: nothing is moved until "
          "the owner approves it in the MyClean tray. Takes only the 'id' values those tools "
          "returned, never paths. No tool can approve or execute a plan.",
          object_schema({{"candidate_ids",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}}},
                           {"minItems", 1},
                           {"maxItems", 64},
                           {"description", "Up to 64 'id' values of file entries"}}}},
                        Json::array({"candidate_ids"}))),
      proposal_tool(
          "propose_optimization",
          "Proposes a change that frees space on the system drive: the hibernation type, moving "
          "the page file, limiting the shadow (restore point) storage, or relocating TEMP or a "
          "package cache to another drive. It only creates a plan in state 'awaiting_approval' "
          "with the exact commands and their undo; nothing changes until the owner approves it "
          "in the MyClean tray. Values left out take MyClean's recommendation.",
          object_schema(
              {{"kind",
                {{"type", "string"},
                 {"enum", {"hibernation", "page_file", "shadow_storage", "relocate"}}}},
               {"mode",
                {{"type", "string"},
                 {"enum", {"off", "reduced", "full"}},
                 {"description", "hibernation: the new type"}}},
               {"target",
                {{"type", "string"},
                 {"description", "page_file, relocate: the target drive letter, such as 'D'"}}},
               {"volume",
                {{"type", "string"},
                 {"description", "shadow_storage: the drive letter (default the system drive)"}}},
               {"maximum_gb",
                {{"type", "number"},
                 {"exclusiveMinimum", 0},
                 {"description", "shadow_storage: the new limit in GB"}}},
               {"cache",
                {{"type", "string"},
                 {"enum",
                  {"temp", "gradle", "cargo", "npm", "nuget", "pip", "go_modules", "go_build",
                   "unreal_ddc"}},
                 {"description", "relocate: what to move"}}},
               {"content",
                {{"type", "string"},
                 {"enum", {"move", "regenerate"}},
                 {"description", "relocate: move the existing content, or let it regenerate"}}}},
              Json::array({"kind"}))),
  });
  return list;
}

bool has_type(const Json& value, std::string_view type) {
  if (type == "array") {
    return value.is_array();
  }
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

// Why the array argument `name` does not fit its `property` ("items" type, "minItems",
// "maxItems"), or an empty string; also for a value that is no array.
std::string check_array(const std::string& name, const Json& property, const Json& value) {
  if (!value.is_array()) {
    return {};
  }
  const std::string item_type = property.at("items").at("type").get<std::string>();
  const std::size_t min_items = property.value("minItems", std::size_t{0});
  const std::size_t max_items = property.value("maxItems", std::size_t{64});
  if (!std::ranges::all_of(value, [&](const Json& item) { return has_type(item, item_type); }) ||
      value.size() < min_items || value.size() > max_items) {
    return std::format("argument '{}' must hold {} to {} values of type {}", name, min_items,
                       max_items, item_type);
  }
  return {};
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
    if (std::string problem = check_array(name, *property, value); !problem.empty()) {
      return problem;
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

// The service method and parameters of a storage, plan or proposal tool; nullopt for the
// other tools. Proposals carry the client's name.
struct ServiceCall {
  std::string_view method;
  Json params;
};

std::optional<ServiceCall> space_or_plan_call(std::string_view tool, const Json& arguments,
                                              const std::string& client) {
  const Json refresh{{"refresh", arguments.value("refresh", false)}};
  if (tool == "storage_top") {
    return ServiceCall{.method = ipc::space_top_method,
                       .params = Json{{"path", arguments.value("path", "C")},
                                      {"n", arguments.value("n", 20)},
                                      {"refresh", arguments.value("refresh", false)}}};
  }
  if (tool == "find_old_downloads") {
    return ServiceCall{.method = ipc::space_old_downloads_method, .params = refresh};
  }
  if (tool == "find_duplicates") {
    return ServiceCall{.method = ipc::space_duplicates_method, .params = refresh};
  }
  if (tool == "find_used_installers") {
    return ServiceCall{.method = ipc::space_used_installers_method, .params = refresh};
  }
  if (tool == "list_plans") {
    return ServiceCall{.method = ipc::plans_list_method, .params = arguments};
  }
  if (tool == "get_plan") {
    return ServiceCall{.method = ipc::plans_get_method, .params = Json{{"id", arguments.at("id")}}};
  }
  if (tool == "propose_cleanup") {
    return ServiceCall{.method = ipc::plans_propose_cleanup_method,
                       .params = Json{{"candidates", {{"ids", arguments.at("candidate_ids")}}},
                                      {"client", client}}};
  }
  if (tool == "propose_optimization") {
    Json proposal = arguments;
    proposal["client"] = client;
    return ServiceCall{.method = ipc::plans_propose_optimization_method,
                       .params = std::move(proposal)};
  }
  return std::nullopt;
}

// The client's name as an MCP actor ("mcp:<name>"): the clientInfo name of initialize reduced to
// a token, "unknown" when it has none.
std::string client_token(const Json& params) {
  constexpr std::size_t max_client = 64;
  const auto info = params.find("clientInfo");
  std::string name;
  if (info != params.end() && info->is_object() && info->contains("name") &&
      info->at("name").is_string()) {
    name = info->at("name").get<std::string>();
  }
  std::string token;
  for (const char c : name.substr(0, max_client)) {
    const bool letter_or_digit =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    token.push_back(letter_or_digit || c == '.' || c == '_' || c == '-' ? c : '-');
  }
  while (!token.empty() && !ipc::is_token(token)) {
    token.erase(token.begin());
  }
  return token.empty() ? std::string("unknown") : token;
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

Json Server::initialize(const Json& params) {
  client_ = client_token(params);
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
       "View of this PC from the MyClean service: live CPU, memory, disk, GPU, volume and "
       "process-tree telemetry and its history, the trust signal of processes "
       "(explain_process), disk health, alerts, copy/paste freezes, the largest files, old "
       "downloads, duplicates, used installers and MyClean's plans. No tool changes anything: "
       "nothing can close, suspend or reprioritize a process, and propose_cleanup and "
       "propose_optimization only create plans that wait for the owner's approval in the "
       "MyClean tray; no tool approves or executes a plan. A process flagged "
       "'unrecognized' is not in MyClean's knowledge base and has a stated reason (unsigned, "
       "invalid signature, signer not listed, user-writable path); describe it with those "
       "reasons and never as dangerous or malicious."}};
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
  if (const std::optional<ServiceCall> mapped = space_or_plan_call(tool_name, arguments, client_);
      mapped) {
    reply = call_(mapped->method, mapped->params);
  } else if (tool_name == "health_snapshot") {
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
  } else if (tool_name == "explain_process") {
    reply = call_(ipc::processes_explain_method, Json{{"pid", arguments.at("pid")}});
  } else if (tool_name == "disk_health") {
    reply = call_(ipc::disks_health_method, Json::object());
  } else if (tool_name == "list_alerts") {
    reply = call_(ipc::alerts_list_method, Json::object());
  } else if (tool_name == "freeze_summary") {
    reply = call_(ipc::input_summary_method, Json::object());
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
