#include "cli/monitor_command.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "cli/command.hpp"
#include "cli/text.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/resident.hpp"
#include "core/mcp/client.hpp"
#include "core/os/clock.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;
using render::bytes;
using render::fixed;
using render::list_of;
using render::number;
using render::text_of;
using render::utc;

// "12 incidents in 168.0 h (1.7 a day)" of a Rate.
std::string rate(const Json& rate) {
  if (!rate.is_object()) {
    return "not measured";
  }
  return std::format("{} incidents in {} h ({} a day)", fixed(number(rate, "incidents"), 0),
                     fixed(number(rate, "hours")), fixed(number(rate, "per_day")));
}

std::string running(const Json& object) {
  const Json value = object.value("running", Json());
  if (!value.is_boolean()) {
    return "";
  }
  return value.get<bool>() ? ", running" : ", not running";
}

std::string pause_line(const Json& pause) {
  std::string out = std::format("  plan {} {} ({}), {} to {}", fixed(number(pause, "plan"), 0),
                                text_of(pause, "image"), text_of(pause, "mode"),
                                utc(pause, "started"), utc(pause, "ends"));
  if (pause.value("end_reason", Json()).is_string()) {
    out += ", " + text_of(pause, "end_reason");
  }
  out += "\n      during: " + rate(pause.value("during", Json())) +
         "; baseline: " + rate(pause.value("baseline", Json()));
  if (pause.value("change", Json()).is_string()) {
    out += "; " + text_of(pause, "change");
  }
  return out + "\n";
}

std::string pauses_text(const Json& reply) {
  std::string out = "Guided freeze elimination:\n";
  if (reply.value("collecting", false)) {
    out += "  Collecting the baseline: pauses are offered once the first freeze is 7 days old.\n";
  } else {
    out += "  Baseline (the 7 days before now): " + rate(reply.value("baseline", Json())) + "\n";
  }
  const Json suspects = list_of(reply, "suspects");
  out += suspects.empty() ? "Suspects: none\n" : "Suspects, in rank order:\n";
  for (const Json& suspect : suspects) {
    out +=
        std::format("  [{}] {} ({}{}){}\n", fixed(number(suspect, "id"), 0),
                    text_of(suspect, "image"), text_of(suspect, "mode"),
                    suspect.value("hook", false) ? ", owns a keyboard hook" : "", running(suspect));
  }
  if (const Json offer = reply.value("offer", Json()); offer.is_object()) {
    out += std::format("Offered: pause {} (suspect {}, {}) for {} h\n", text_of(offer, "image"),
                       fixed(number(offer, "suspect"), 0), text_of(offer, "mode"),
                       fixed(number(offer, "hours"), 0));
  }
  if (const Json active = reply.value("active", Json()); active.is_object()) {
    out += "Active pause" + running(active) + ":\n" + pause_line(active);
  }
  const Json pauses = list_of(reply, "pauses");
  out += pauses.empty() ? "" : "Pauses, newest first:\n";
  for (const Json& pause : pauses) {
    out += pause_line(pause);
  }
  return out + "A pause is proposed and approved in the tray's Freezes screen.\n";
}

std::string badge_text(const Json& reply) {
  return std::format("Alerts: {} active and not snoozed, worst {}\n",
                     fixed(number(reply, "count"), 0), text_of(reply, "worst"));
}

std::string costs_text(const Json& reply) {
  const Json programs = list_of(reply, "programs");
  std::string out =
      std::format("Resident programs of the last {} days (since {}), most CPU time first:\n",
                  fixed(number(reply, "days"), 0), utc(reply, "from"));
  out += programs.empty() ? "  none ran 24 hours of them yet\n" : "";
  for (const Json& program : programs) {
    out += std::format(
        "  {} (key {}): {} h running, CPU {} s\n      RAM {} average, {} peak; "
        "disk {}, network {}\n",
        text_of(program, "image"), text_of(program, "key"), fixed(number(program, "hours_running")),
        fixed(number(program, "cpu_seconds"), 0), bytes(number(program, "ram_average")),
        bytes(number(program, "ram_peak")), bytes(number(program, "disk_bytes")),
        bytes(number(program, "net_bytes")));
  }
  return out;
}

std::string trend_text(const Json& reply) {
  const Json points = list_of(reply, "points");
  std::string out = std::format("Private bytes of {} (key {}) over the last 48 hours, {} points\n",
                                text_of(reply, "image"), text_of(reply, "key"), points.size());
  if (!points.empty()) {
    const auto [lowest, highest] = std::ranges::minmax_element(
        points, {}, [](const Json& point) { return number(point, "private_bytes").value_or(0.0); });
    out += std::format("  {} at {}, {} at {}; lowest {}, highest {}\n",
                       bytes(number(points.front(), "private_bytes")), utc(points.front(), "time"),
                       bytes(number(points.back(), "private_bytes")), utc(points.back(), "time"),
                       bytes(number(*lowest, "private_bytes")),
                       bytes(number(*highest, "private_bytes")));
  }
  const Json leak = reply.value("leak", Json());
  if (!leak.is_object()) {
    return out + "  No steady growth.\n";
  }
  std::string hourly;
  for (const Json& value : list_of(leak, "hourly")) {
    hourly += (hourly.empty() ? "" : ", ") +
              bytes(value.is_number() ? std::optional(value.get<double>()) : std::nullopt);
  }
  return out + std::format("  Steady growth for {} hours; hourly medians: {}\n",
                           fixed(number(leak, "hours"), 0), hourly);
}

// governor events [--hours <n>] [--limit <n>]
int events(const CommandLine& line, const mcp::Caller& call, const os::IClock& clock,
           std::ostream& out, std::ostream& err) {
  const Result<std::optional<std::int64_t>> since = since_option(line, clock);
  const Result<std::optional<std::int64_t>> limit = option<std::int64_t>(line, "limit");
  if (!since || !limit) {
    err << "myclean: " << (since ? limit.error() : since.error()).message << '\n';
    return exit_usage;
  }
  Json params = Json::object();
  if (*since) {
    params["since"] = **since;
  }
  if (*limit) {
    params["limit"] = **limit;
  }
  return show(call(ipc::governor_events_method, params), line.json, out, err,
              &text::governor_events);
}

// resident costs | trend <key>
int resident(const CommandLine& line, const mcp::Caller& call, std::ostream& out,
             std::ostream& err) {
  const std::vector<std::string>& words = line.words;
  if (words.size() == 2 && words[1] == "costs") {
    return show(call(ipc::resident_costs_method, Json::object()), line.json, out, err, &costs_text);
  }
  if (words.size() == 3 && words[1] == "trend") {
    if (!ipc::is_token(words[2])) {
      err << "myclean: resident trend: not a program key: " << words[2] << '\n';
      return exit_usage;
    }
    return show(call(ipc::resident_trend_method, Json{{"key", words[2]}}), line.json, out, err,
                &trend_text);
  }
  err << "myclean: resident: costs | trend <key>\n";
  return exit_usage;
}

}  // namespace

std::optional<int> monitor_command(const CommandLine& line, const mcp::Caller& call,
                                   const os::IClock& clock, std::ostream& out, std::ostream& err) {
  const std::vector<std::string>& words = line.words;
  if (words.size() == 2 && words[0] == "governor" && words[1] == "events") {
    return events(line, call, clock, out, err);
  }
  if (words.size() == 2 && words[0] == "freezes" && words[1] == "pauses") {
    return show(call(ipc::input_pauses_method, Json::object()), line.json, out, err, &pauses_text);
  }
  if (words.size() == 2 && words[0] == "alerts" && words[1] == "badge") {
    return show(call(ipc::alerts_badge_method, Json::object()), line.json, out, err, &badge_text);
  }
  if (words.front() == "resident") {
    return resident(line, call, out, err);
  }
  return std::nullopt;
}

}  // namespace myclean::cli
