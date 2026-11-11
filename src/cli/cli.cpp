#include "cli/cli.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/map_command.hpp"
#include "cli/packs_command.hpp"
#include "cli/scan_all_command.hpp"
#include "cli/security_command.hpp"
#include "cli/text.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/mcp/client.hpp"
#include "core/mcp/server.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "platform/ipc/pipe_server.hpp"

namespace myclean::cli {

namespace {

using Json = nlohmann::json;

std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// A parsed command line: the command words, --json, and the options that take a value.
struct CommandLine {
  std::vector<std::string> words;
  bool json = false;
  std::vector<std::pair<std::string, std::string>> options;  // name without "--", value
};

// The value of option `name`, or null.
const std::string* find_option(const CommandLine& line, std::string_view name) {
  const auto found =
      std::ranges::find(line.options, name, &std::pair<std::string, std::string>::first);
  return found == line.options.end() ? nullptr : &found->second;
}

Result<CommandLine> parse(std::span<const std::string> args) {
  constexpr std::array<std::string_view, 9> valued{
      "pipe", "sort", "limit", "entity", "hours", "max-points", "min-gb", "depth", "data"};
  CommandLine line;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string& arg = args[index];
    if (arg == "--json") {
      line.json = true;
    } else if (arg.starts_with("--")) {
      const std::string name = arg.substr(2);
      if (std::ranges::find(valued, name) == valued.end() || index + 1 >= args.size()) {
        return fail(Errc::invalid_argument, "unknown or incomplete option " + arg);
      }
      line.options.emplace_back(name, args[++index]);
    } else {
      line.words.push_back(arg);
    }
  }
  if (line.words.empty()) {
    return fail(Errc::invalid_argument, "no command");
  }
  return line;
}

template <typename T>
Result<std::optional<T>> option(const CommandLine& line, std::string_view name) {
  const std::string* text = find_option(line, name);
  if (text == nullptr) {
    return std::optional<T>{};
  }
  const char* const last = std::to_address(text->end());
  T value{};
  const auto [end, error] = std::from_chars(text->data(), last, value);
  if (error != std::errc{} || end != last) {
    return fail(Errc::invalid_argument, "--" + std::string(name) + ": not a number: " + *text);
  }
  return std::optional<T>{value};
}

// Prints a failed call and returns exit_unavailable.
int report(const mcp::CallError& error, bool json, std::ostream& out, std::ostream& err) {
  if (json) {
    out << dump(error.reached ? Json{{"error", error.message}}
                              : Json{{"health", "unreachable"}, {"reason", error.message}})
        << '\n';
  } else {
    err << "myclean: " << (error.reached ? error.message : mcp::unavailable_text(error.message))
        << '\n';
  }
  return exit_unavailable;
}

int status(const mcp::Caller& call, bool json, std::ostream& out, std::ostream& err) {
  const mcp::Reply reply = call(ipc::status_method, Json::object());
  if (!reply) {
    return report(reply.error(), json, out, err);
  }
  const Result<ipc::StatusReply> status = ipc::parse_status(*reply);
  if (!status) {
    return report(mcp::CallError{.reached = true, .message = status.error().message}, json, out,
                  err);
  }
  Json document = *reply;
  if (!status->healthy) {
    std::string reason = "degraded:";
    if (status->store.error) {
      reason += " store: " + *status->store.error + ";";
    }
    for (const ipc::ComponentHealth& component : status->component_health) {
      for (const std::string& problem : component.problems) {
        reason += " " + component.name + ": " + problem + ";";
      }
    }
    reason.pop_back();
    document["reason"] = reason;
  }
  out << (json ? dump(document) + "\n" : text::status(document));
  return status->healthy ? exit_ok : exit_degraded;
}

// Calls `method` and prints its result as JSON or as `render` makes it.
int show(const mcp::Reply& reply, bool json, std::ostream& out, std::ostream& err,
         std::string (*render)(const Json&)) {
  if (!reply) {
    return report(reply.error(), json, out, err);
  }
  out << (json ? dump(*reply) + "\n" : render(*reply));
  return exit_ok;
}

std::string render_volumes(const Json& reply) {
  return text::volumes(reply.value("volumes", Json::array()));
}

// metrics query <metric> [--entity <entity>] [--hours <n>] [--max-points <n>]
int query(const CommandLine& line, const mcp::Caller& call, const os::IClock& clock,
          std::ostream& out, std::ostream& err) {
  const Result<std::optional<double>> hours = option<double>(line, "hours");
  const Result<std::optional<std::int64_t>> points = option<std::int64_t>(line, "max-points");
  if (!hours || !points) {
    err << "myclean: " << (hours ? points.error() : hours.error()).message << '\n';
    return exit_usage;
  }
  const auto now =
      std::chrono::duration_cast<std::chrono::seconds>(clock.now().time_since_epoch()).count();
  const auto span = static_cast<std::int64_t>(hours->value_or(1.0) * 3600.0);
  const std::string* entity = find_option(line, "entity");
  return show(
      call(ipc::metrics_query_method, Json{{"metric", line.words[2]},
                                           {"entity", entity != nullptr ? *entity : "system"},
                                           {"from", now - std::max<std::int64_t>(span, 1)},
                                           {"max_points", points->value_or(1000)}}),
      line.json, out, err, &text::history);
}

// A one-word command that calls one method without parameters.
struct PlainCommand {
  std::string_view name;
  std::string_view method;
  std::string (*render)(const Json&);
};

constexpr std::array<PlainCommand, 5> plain_commands{
    PlainCommand{.name = "volumes", .method = ipc::volumes_list_method, .render = &render_volumes},
    PlainCommand{.name = "disks", .method = ipc::disks_health_method, .render = &text::disks},
    PlainCommand{.name = "alerts", .method = ipc::alerts_list_method, .render = &text::alerts},
    PlainCommand{.name = "freezes", .method = ipc::input_summary_method, .render = &text::freezes},
    PlainCommand{
        .name = "governor", .method = ipc::governor_status_method, .render = &text::governor}};

// explain <pid>
int explain(const CommandLine& line, const mcp::Caller& call, std::ostream& out,
            std::ostream& err) {
  const std::string& text = line.words[1];
  const char* const last = std::to_address(text.end());
  std::int64_t pid = 0;
  const auto [end, error] = std::from_chars(text.data(), last, pid);
  if (error != std::errc{} || end != last || pid < 0) {
    err << "myclean: explain: not a process id: " << text << '\n';
    return exit_usage;
  }
  return show(call(ipc::processes_explain_method, Json{{"pid", pid}}), line.json, out, err,
              &text::explain);
}

// map [<volume-or-path>] [--min-gb <n>] [--depth <n>] [--data <dir>]
int map_volume(const CommandLine& line, std::ostream& out, std::ostream& err) {
  const Result<std::optional<double>> min_gb = option<double>(line, "min-gb");
  const Result<std::optional<int>> depth = option<int>(line, "depth");
  if (!min_gb || !depth) {
    err << "myclean: " << (min_gb ? depth.error() : min_gb.error()).message << '\n';
    return exit_usage;
  }
  MapRequest request{
      .min_gb = min_gb->value_or(1.0), .depth = depth->value_or(6), .json = line.json};
  if (!(request.min_gb >= 0.0) || request.depth < 0) {
    err << "myclean: map: --min-gb and --depth must not be negative\n";
    return exit_usage;
  }
  if (line.words.size() == 2) {
    request.target = line.words[1];
  }
  if (const std::string* data = find_option(line, "data"); data != nullptr) {
    request.data_dir = std::filesystem::path{std::u8string(data->begin(), data->end())};
  }
  return map_command(request, out, err);
}

int dispatch(const CommandLine& line, const mcp::Caller& call, const os::IClock& clock,
             std::istream& in, std::ostream& out, std::ostream& err) {
  const std::vector<std::string>& words = line.words;
  const std::string& command = words.front();
  if (command == "status" && words.size() == 1) {
    return status(call, line.json, out, err);
  }
  if (command == "health" && words.size() == 1) {
    return show(mcp::health_snapshot(call), line.json, out, err, &text::health);
  }
  if (words.size() == 1) {
    for (const PlainCommand& plain : plain_commands) {
      if (command == plain.name) {
        return show(call(plain.method, Json::object()), line.json, out, err, plain.render);
      }
    }
  }
  if (command == "processes" && words.size() == 1) {
    const Result<std::optional<std::int64_t>> limit = option<std::int64_t>(line, "limit");
    if (!limit) {
      err << "myclean: " << limit.error().message << '\n';
      return exit_usage;
    }
    Json params{{"limit", limit->value_or(10)}};
    if (const std::string* sort = find_option(line, "sort"); sort != nullptr) {
      params["sort"] = *sort;
    }
    return show(call(ipc::processes_top_method, params), line.json, out, err, &text::trees);
  }
  if (command == "explain" && words.size() == 2) {
    return explain(line, call, out, err);
  }
  if (command == "metrics" && words.size() == 2 && words[1] == "latest") {
    return show(call(ipc::metrics_latest_method, Json::object()), line.json, out, err,
                &text::system);
  }
  if (command == "metrics" && words.size() == 3 && words[1] == "query") {
    return query(line, call, clock, out, err);
  }
  if (command == "mcp" && words.size() == 1) {
    mcp::Server server(call, clock, MYCLEAN_VERSION);
    server.serve(in, out);
    return exit_ok;
  }
  err << "myclean: unknown command\n" << usage();
  return exit_usage;
}

}  // namespace

std::string_view usage() {
  return "usage: myclean [--pipe <name>] <command> [--json]\n"
         "  status                      service health; exit 0 healthy, 1 unreachable,\n"
         "                              2 degraded\n"
         "  health                      process trees above 5 % CPU or memory, and a system\n"
         "                              summary\n"
         "  processes [--sort cpu|memory] [--limit <n>]\n"
         "                              the top process trees, with their trust signal\n"
         "  explain <pid>               one process: tree, use, signer, path, trust and\n"
         "                              knowledge-base entry\n"
         "  disks                       health of every physical disk\n"
         "  alerts                      stored alerts, newest first\n"
         "  freezes                     copy/paste freezes grouped by mechanism and process\n"
         "  governor                    the resource governor: mode, pressure, game mode, jobs,\n"
         "                              steps in effect and recent events\n"
         "  volumes                     volumes with free space and days until full\n"
         "  metrics latest              the newest CPU, memory, disk and GPU values\n"
         "  metrics query <metric> [--entity <entity>] [--hours <n>] [--max-points <n>]\n"
         "                              history of one metric over the last hours\n"
         "  map [<volume-or-path>] [--min-gb <n>] [--depth <n>] [--data <dir>]\n"
         "                              scans here (no service needed, low priority) and\n"
         "                              explains what uses the space and how to reclaim it;\n"
         "                              default: the system drive, 1 GiB, 6 levels\n"
         "  packs list | validate [<path>] [--data <dir>]\n"
         "                              the knowledge packs and whether they apply here; or\n"
         "                              validate packs and run their fixtures (no service)\n"
         "  security scan | extensions | network [--hours <n>]\n"
         "                              the intruder scan: persistence points with their\n"
         "                              verdict or reasons, browser extensions with their\n"
         "                              origin, processes that used the network\n"
         "  security defender-scan <id> Microsoft Defender scans the file of scan item <id>;\n"
         "                              it reports only and removes nothing\n"
         "  mcp                         MCP server on stdin/stdout with read-only tools\n"
         "--json writes one JSON document to stdout; --pipe <name> talks to \\\\.\\pipe\\<name>\n"
         "instead of the service's pipe. Nothing here changes the system.\n";
}

mcp::Caller pipe_caller(std::wstring pipe, std::chrono::milliseconds timeout) {
  return
      [pipe = std::move(pipe), timeout](std::string_view method, const Json& params) -> mcp::Reply {
        Result<platform::PipeClient> client = platform::PipeClient::connect(pipe, timeout);
        if (!client) {
          return tl::unexpected(mcp::CallError{
              .reached = false,
              .message = client.error().code == Errc::not_found
                             ? "the service is not running: nothing listens on its pipe"
                             : client.error().message});
        }
        const Result<std::string> response = client->exchange(ipc::make_request(1, method, params));
        if (!response) {
          const bool timed_out = response.error().message.ends_with("timed out");
          return tl::unexpected(mcp::CallError{
              .reached = false,
              .message = timed_out ? "timeout: the service accepted the connection but did not "
                                     "answer within " +
                                         std::to_string(timeout.count() / 1000) + " s"
                                   : response.error().message});
        }
        Result<Json> result = ipc::parse_response(*response, 1);
        if (!result) {
          return tl::unexpected(mcp::CallError{.reached = true, .message = result.error().message});
        }
        return std::move(*result);
      };
}

int run(std::span<const std::string> args, std::istream& in, std::ostream& out, std::ostream& err,
        const os::IClock& clock, std::chrono::milliseconds timeout) {
  if (!args.empty() && args.front() == "scan-all") {
    return scan_all_command(args.subspan(1), out, err);  // runs here, without the service
  }
  if (!args.empty() && args.front() == "packs") {
    return packs_command(args.subspan(1), out, err);  // runs here, without the service
  }
  const Result<CommandLine> line = parse(args);
  if (!line) {
    err << "myclean: " << line.error().message << '\n' << usage();
    return exit_usage;
  }
  if (line->words.front() == "map" && line->words.size() <= 2) {
    return map_volume(*line, out, err);  // runs here, without the service
  }
  std::wstring pipe(platform::default_pipe_name);
  if (const std::string* name = find_option(*line, "pipe"); name != nullptr) {
    if (!ipc::is_token(*name)) {
      err << "myclean: --pipe: the name must be a token\n";
      return exit_usage;
    }
    pipe = LR"(\\.\pipe\)" + std::wstring(name->begin(), name->end());
  }
  if (line->words.front() == "security" && line->words.size() >= 2) {
    return security_command(std::span<const std::string>(line->words).subspan(1),
                            find_option(*line, "hours"), line->json,
                            pipe_caller(std::move(pipe), timeout), out, err);
  }
  return dispatch(*line, pipe_caller(std::move(pipe), timeout), clock, in, out, err);
}

}  // namespace myclean::cli
