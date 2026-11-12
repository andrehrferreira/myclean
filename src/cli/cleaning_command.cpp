#include "cli/cleaning_command.hpp"

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
#include "core/ipc/plans.hpp"
#include "core/mcp/client.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;
using render::bytes;
using render::fixed;
using render::list_of;
using render::number;
using render::text_of;
using render::utc;

std::string seconds(const Json& object, std::string_view key) {
  return fixed(number(object, key)) + " s";
}

// "name (source, scope)" of a startup item reference.
std::string item_name(const Json& item) {
  return std::format("{} ({}, {})", text_of(item, "name"), text_of(item, "source"),
                     text_of(item, "scope"));
}

std::string impact_text(const Json& impact) {
  if (!impact.is_object()) {
    return "impact not known";
  }
  if (text_of(impact, "source") == "diagnostics") {
    return "delayed a boot by up to " + seconds(impact, "delay_seconds") +
           " (Windows diagnostics, last 30 days)";
  }
  return std::format(
      "{} CPU and {} of disk in the first 2 minutes after logon (median of {} "
      "logons)",
      seconds(impact, "cpu_seconds"), bytes(number(impact, "io_bytes")),
      fixed(number(impact, "logons"), 0));
}

// The text of a JSON boolean, or `unknown` for null.
std::string flag(const Json& value, const char* yes, const char* no, const char* unknown) {
  if (!value.is_boolean()) {
    return unknown;
  }
  return value.get<bool>() ? yes : no;
}

std::string startup_text(const Json& reply) {
  std::string out = "Startup items:\n";
  const Json items = list_of(reply, "items");
  out += items.empty() ? "  none\n" : "";
  for (const Json& item : items) {
    const std::string state = flag(item.value("enabled", Json()), "on", "off", "once");
    out +=
        std::format("  [{}] {:<4} {} ({}, {}){}{}\n", fixed(number(item, "id"), 0), state,
                    text_of(item, "display_name"), text_of(item, "source"), text_of(item, "scope"),
                    item.contains("signer") ? ", signed by " + text_of(item, "signer") : "",
                    item.contains("start") ? ", start " + text_of(item, "start") +
                                                 (item.value("delayed", false) ? " (delayed)" : "")
                                           : "");
    out += "      " + text_of(item, "command") + "\n";
    out += "      " + impact_text(item.value("impact", Json())) + "\n";
  }
  return out +
         "\"once\" items run once and cannot be turned off; a change is proposed in the "
         "tray's Startup screen.\n";
}

// "+3.0 s" from the median, or "" without one.
std::string difference(const Json& object, std::string_view key) {
  const std::optional<double> value = number(object, key);
  return value ? std::format(" ({:+.1f} s from the median)", *value) : "";
}

std::string latest_boot(const Json& latest) {
  std::string out = std::format(
      "Latest boot {}: boot {}{} (main path {}, post-boot {}), logon {}{}\n", utc(latest, "start"),
      seconds(latest, "boot_seconds"), difference(latest, "boot_difference_seconds"),
      seconds(latest, "main_path_seconds"), seconds(latest, "post_boot_seconds"),
      seconds(latest, "logon_seconds"), difference(latest, "logon_difference_seconds"));
  const Json contributors = list_of(latest, "contributors");
  if (!contributors.empty()) {
    out += "What ran in the first 2 minutes of that logon:\n";
  }
  for (const Json& contributor : contributors) {
    const Json item = contributor.value("item", Json());
    out += std::format("  {:<28} CPU {}, disk {}{}\n", text_of(contributor, "image"),
                       seconds(contributor, "cpu_seconds"), bytes(number(contributor, "io_bytes")),
                       item.is_object() ? ", startup item " + item_name(item) : "");
  }
  return out;
}

// "20.0 s (5 boots)" of a before or after median, or why there is none.
std::string logon_median(const Json& median) {
  return median.is_object() ? std::format("{} ({} boots)", seconds(median, "logon_seconds"),
                                          fixed(number(median, "boots"), 0))
                            : "not measured yet";
}

std::string boot_times_text(const Json& reply) {
  std::string out = "Boot and logon times, as Windows measured them:\n";
  if (reply.value("log_error", Json()).is_string()) {
    out += "  The diagnostics log could not be read now: " + text_of(reply, "log_error") + "\n";
  }
  const Json latest = reply.value("latest", Json());
  out += latest.is_object() ? latest_boot(latest) : "No boot recorded yet.\n";
  const Json median = reply.value("median", Json());
  if (median.is_object()) {
    out += std::format("Median over {} boots of the last 30 days: boot {}, logon {}\n",
                       fixed(number(median, "boots"), 0), seconds(median, "boot_seconds"),
                       seconds(median, "logon_seconds"));
  }
  for (const Json& change : list_of(reply, "changes")) {
    std::string items;
    for (const Json& item : list_of(change, "items")) {
      items += (items.empty() ? "" : ", ") + item_name(item);
    }
    out += std::format("Plan {} applied {} disabled {}\n  logon before {}, after {}\n",
                       fixed(number(change, "plan"), 0), utc(change, "applied"), items,
                       logon_median(change.value("before", Json())),
                       logon_median(change.value("after", Json())));
  }
  return out;
}

// One plan summary line.
std::string plan_line(const Json& plan) {
  const Json verification = plan.value("verification", Json());
  return std::format(
      "  [{}] {:<17} {:<14} {}: {} items, {}, by {}, {}{}{}\n", fixed(number(plan, "id"), 0),
      text_of(plan, "state"), text_of(plan, "kind"), text_of(plan, "title"),
      fixed(number(plan, "items"), 0), bytes(number(plan, "bytes")), text_of(plan, "proposer"),
      utc(plan, "created_at"),
      verification.is_string() ? ", verified " + verification.get<std::string>() : "",
      plan.value("needs_restart", false) ? ", needs a restart" : "");
}

std::string plans_text(const Json& reply) {
  const Json plans = list_of(reply, "plans");
  std::string out = "Plans, newest first:\n";
  out += plans.empty() ? "  none\n" : "";
  for (const Json& plan : plans) {
    out += plan_line(plan);
  }
  return out;
}

std::string verification_text(const Json& verification) {
  if (!verification.is_object()) {
    return "not applied yet";
  }
  std::string out = text_of(verification, "result");
  if (verification.value("metric", Json()).is_string()) {
    out += std::format(" ({}: {} before, {} after)", text_of(verification, "metric"),
                       fixed(number(verification, "before"), 2),
                       fixed(number(verification, "after"), 2));
  }
  if (verification.value("detail", Json()).is_string()) {
    out += "; " + text_of(verification, "detail");
  }
  return out + ", " + utc(verification, "time");
}

std::string item_line(const Json& item) {
  const std::string decision =
      flag(item.value("approved", Json()), "approved", "rejected", "not decided");
  return std::format(
      "    [{}] {:>10}  {}\n        {}{}; {}\n", fixed(number(item, "id"), 0),
      bytes(number(item, "bytes")), text_of(item, "path"),
      item.value("rule", Json()).is_string() ? "rule " + text_of(item, "rule") + ", " : "",
      text_of(item, "reason"), decision);
}

std::string plan_text(const Json& plan) {
  std::string out =
      std::format("Plan {}: {}\n", fixed(number(plan, "id"), 0), text_of(plan, "title"));
  out += std::format("  {} ({} tier, runs in the {}), {}, proposed by {} {}\n",
                     text_of(plan, "kind"), text_of(plan, "tier"), text_of(plan, "scope"),
                     text_of(plan, "state"), text_of(plan, "proposer"), utc(plan, "created_at"));
  out += std::format(
      "  {} items, {}{}{}\n", fixed(number(plan, "items_total"), 0), bytes(number(plan, "bytes")),
      plan.value("freed", Json()).is_number() ? ", freed " + bytes(number(plan, "freed")) : "",
      plan.value("needs_restart", false) ? ", needs a restart" : "");
  for (const Json& command : list_of(plan, "commands")) {
    out += "  runs: " + (command.is_string() ? command.get<std::string>() : dump(command)) + "\n";
  }
  out += "  undo: " + text_of(plan, "undo") + "\n";
  for (const Json& warning : list_of(plan, "warnings")) {
    out +=
        "  warning: " + (warning.is_string() ? warning.get<std::string>() : dump(warning)) + "\n";
  }
  if (plan.value("detail", Json()).is_string()) {
    out += "  detail: " + text_of(plan, "detail") + "\n";
  }
  out += "  verification: " + verification_text(plan.value("verification", Json())) + "\n";
  const Json items = list_of(plan, "items");
  out += "  Items:\n";
  for (const Json& item : items) {
    out += item_line(item);
  }
  const std::optional<double> total = number(plan, "items_total");
  if (total && *total > static_cast<double>(items.size())) {
    out += std::format("    ... and {:.0f} more items not listed\n",
                       *total - static_cast<double>(items.size()));
  }
  return out;
}

std::string quarantine_text(const Json& reply) {
  const Json runs = list_of(reply, "runs");
  std::string out = "Quarantine (each run is deleted for good after its purge date):\n";
  out += runs.empty() ? "  nothing held\n" : "";
  for (const Json& run : runs) {
    out += std::format("  plan {}, moved {}, purged after {}, {}\n", fixed(number(run, "plan"), 0),
                       utc(run, "moved_at"), utc(run, "purge_after"), bytes(number(run, "bytes")));
    for (const Json& item : list_of(run, "items")) {
      out += std::format("    [{}] {:>10}  {}\n        held at {}\n",
                         fixed(number(item, "item"), 0), bytes(number(item, "bytes")),
                         text_of(item, "original"), text_of(item, "location"));
    }
  }
  return out + "Restoring or purging a run is done in the tray.\n";
}

std::string applying_text(const Json& applying) {
  const bool plan = text_of(applying, "kind") == "plan";
  std::string out = plan
                        ? std::format("Applying plan {} \"{}\"", fixed(number(applying, "plan"), 0),
                                      text_of(applying, "title"))
                        : "Purging " + text_of(applying, "title");
  out +=
      std::format(" since {}: {} of {} items, {} of {}, {} files", utc(applying, "started_at"),
                  fixed(number(applying, "items_done"), 0),
                  fixed(number(applying, "items_total"), 0), bytes(number(applying, "bytes_done")),
                  bytes(number(applying, "bytes_total")), fixed(number(applying, "files_done"), 0));
  if (applying.value("current", Json()).is_string()) {
    out += "\n  now: " + text_of(applying, "current");
  }
  return out + "\n";
}

std::string time_or(const Json& object, std::string_view key, const char* otherwise) {
  return object.value(key, Json()).is_number() ? utc(object, key) : std::string(otherwise);
}

std::string cleaning_text(const Json& reply) {
  const std::optional<double> progress = number(reply, "progress");
  std::string out =
      text_of(reply, "state") == "scanning"
          ? std::format("Cleaning: scanning ({} %)\n",
                        fixed(progress ? std::optional(*progress * 100.0) : std::nullopt, 0))
          : "Cleaning: idle\n";
  out += std::format("  last scan completed {}, next automatic run {}\n",
                     time_or(reply, "last_completed", "never"),
                     time_or(reply, "next_auto_run", "not planned"));
  out += std::format("  {} candidates, {}\n", fixed(number(reply, "candidates"), 0),
                     bytes(number(reply, "bytes")));
  if (const Json applying = reply.value("applying", Json()); applying.is_object()) {
    out += "  " + applying_text(applying);
  }
  std::string purging;
  for (const Json& run : list_of(reply, "purging")) {
    purging += (purging.empty() ? "" : ", ") + dump(run);
  }
  if (!purging.empty()) {
    out += "  quarantine runs queued for purging: " + purging + "\n";
  }
  if (reply.value("unavailable", Json()).is_string()) {
    out += "  unavailable: " + text_of(reply, "unavailable") + "\n";
  }
  return out;
}

std::string system_space_text(const Json& reply) {
  std::string out = "System settings that hold space on the system drive:\n";
  for (const Json& setting : list_of(reply, "settings")) {
    out += "  " + text_of(setting, "kind") + ": ";
    if (setting.value("error", Json()).is_string()) {
      out += "could not be read: " + text_of(setting, "error") + "\n";
      continue;
    }
    out += std::format("{}, {} on disk; ", text_of(setting, "current"),
                       bytes(number(setting, "cost")));
    out += setting.value("recommended", Json()).is_string()
               ? "recommended: " + text_of(setting, "recommended") +
                     (setting.value("restart_required", false) ? " (needs a restart)" : "")
               : std::string("no change recommended");
    out += "\n";
  }
  return out + "A change is a proposal that waits for your approval in the tray.\n";
}

int startup(const CommandLine& line, const mcp::Caller& call, std::ostream& out,
            std::ostream& err) {
  if (line.words.size() == 2 && line.words[1] == "list") {
    return show(call(ipc::startup_list_method, Json::object()), line.json, out, err, &startup_text);
  }
  if (line.words.size() == 2 && line.words[1] == "boot-times") {
    return show(call(ipc::startup_boot_times_method, Json::object()), line.json, out, err,
                &boot_times_text);
  }
  err << "myclean: startup: list | boot-times\n";
  return exit_usage;
}

int plans(const CommandLine& line, const mcp::Caller& call, std::ostream& out, std::ostream& err) {
  if (line.words.size() == 2 && line.words[1] == "list") {
    Json params = Json::object();
    if (const std::string* state = find_option(line, "state"); state != nullptr) {
      params["state"] = *state;
    }
    return show(call(ipc::plans_list_method, params), line.json, out, err, &plans_text);
  }
  if (line.words.size() == 3 && line.words[1] == "get") {
    const std::optional<std::int64_t> id = positive_id(line.words[2]);
    if (!id) {
      err << "myclean: plans get: not a plan id: " << line.words[2] << '\n';
      return exit_usage;
    }
    return show(call(ipc::plans_get_method, Json{{"id", *id}}), line.json, out, err, &plan_text);
  }
  err << "myclean: plans: list [--state <state>] | get <id>\n";
  return exit_usage;
}

}  // namespace

std::optional<int> cleaning_command(const CommandLine& line, const mcp::Caller& call,
                                    std::ostream& out, std::ostream& err) {
  const std::string& command = line.words.front();
  if (command == "startup") {
    return startup(line, call, out, err);
  }
  if (command == "plans") {
    return plans(line, call, out, err);
  }
  if (command == "cleaning" && line.words.size() == 1) {
    return show(call(ipc::cleaning_status_method, Json::object()), line.json, out, err,
                &cleaning_text);
  }
  if (command == "system-space" && line.words.size() == 1) {
    return show(call(ipc::system_space_status_method, Json::object()), line.json, out, err,
                &system_space_text);
  }
  if (command == "quarantine" && line.words.size() == 1) {
    return show(call(ipc::quarantine_list_method, Json::object()), line.json, out, err,
                &quarantine_text);
  }
  return std::nullopt;
}

}  // namespace myclean::cli
