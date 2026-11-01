#include "cli/text.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace myclean::cli::text {

namespace {

using Json = nlohmann::json;

std::optional<double> number(const Json& object, std::string_view key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto found = object.find(key);
  return found != object.end() && found->is_number() ? std::optional(found->get<double>())
                                                     : std::nullopt;
}

std::string text_of(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>() : "n/a";
}

std::string bytes(std::optional<double> value) {
  if (!value) {
    return "n/a";
  }
  constexpr std::array<std::string_view, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
  double size = *value;
  std::size_t unit = 0;
  while (std::abs(size) >= 1024.0 && unit + 1 < units.size()) {
    size /= 1024.0;
    ++unit;
  }
  return unit == 0 ? std::format("{:.0f} B", size) : std::format("{:.1f} {}", size, units.at(unit));
}

std::string fixed(std::optional<double> value, int decimals = 1) {
  return value ? std::format("{:.{}f}", *value, decimals) : "n/a";
}

std::string utc(const Json& object, std::string_view key) {
  const std::optional<double> seconds = number(object, key);
  if (!seconds) {
    return "n/a";
  }
  const std::chrono::sys_seconds time{std::chrono::seconds{static_cast<std::int64_t>(*seconds)}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC", time);
}

std::string fill(const Json& volume) {
  const Json& projection = volume.value("fill", Json::object());
  const std::string state = text_of(projection, "state");
  if (state == "filling") {
    return std::format("filling, full in {} days", fixed(number(projection, "days_until_full"), 0));
  }
  return state == "not_filling" ? "not filling" : "trend not known yet";
}

// One line on the trust signal of a process: facts only, never a verdict.
std::string trust_line(const Json& trust) {
  if (!trust.is_object()) {
    return "trust: not known (the image cannot be read)";
  }
  std::string out = "trust: " + text_of(trust, "status");
  if (trust.value("signer", Json()).is_string()) {
    out += " by " + text_of(trust, "signer");
  }
  out += ", " + text_of(trust, "path_class") + " path";
  if (trust.value("unrecognized", false)) {
    std::string reasons;
    for (const Json& reason : trust.value("reasons", Json::array())) {
      reasons += (reasons.empty() ? "" : ", ") + reason.get<std::string>();
    }
    out += "; UNRECOGNIZED (" + reasons + ")";
  }
  out += "; " + (trust.value("description", Json()).is_string() ? text_of(trust, "description")
                                                                : std::string("unknown"));
  return out;
}

std::string optional_integer(const Json& object, std::string_view key, std::string_view unit) {
  const std::optional<double> value = number(object, key);
  return value ? std::format("{:.0f}{}", *value, unit) : "n/a";
}

}  // namespace

std::string status(const Json& status) {
  std::string out = std::format(
      "MyClean service: {} (version {}, build {}, {} mode, pid {}, up {} s)\n",
      text_of(status, "health"),
      status.value("version", Json()).is_string() ? text_of(status, "version") : "development",
      text_of(status, "build"), text_of(status, "mode"), fixed(number(status, "pid"), 0),
      fixed(number(status, "uptime_s"), 0));
  const Json& store = status.value("store", Json::object());
  out +=
      std::format("  store {}{}\n", text_of(store, "path"),
                  store.value("error", Json()).is_string() ? ": " + text_of(store, "error") : "");
  for (const Json& component : status.value("component_health", Json::array())) {
    out += std::format("  {}: {}\n", text_of(component, "name"), text_of(component, "health"));
    for (const Json& problem : component.value("problems", Json::array())) {
      out += "    problem: " + problem.get<std::string>() + "\n";
    }
    for (const Json& missing : component.value("unavailable", Json::array())) {
      out += "    unavailable: " + missing.get<std::string>() + "\n";
    }
  }
  return out;
}

std::string trees(const Json& top) {
  const Json& list = top.value("trees", Json::array());
  std::string out;
  if (top.value("filter", Json()).is_object()) {
    out = std::format("Process trees above {} % CPU or {} % of memory ({} of {} trees):\n",
                      fixed(number(top["filter"], "cpu_percent"), 0),
                      fixed(number(top["filter"], "memory_percent"), 0), list.size(),
                      fixed(number(top, "total_trees"), 0));
  } else {
    out = std::format("Top process trees ({} of {}):\n", list.size(),
                      fixed(number(top, "total_trees"), 0));
  }
  if (list.empty()) {
    out += "  none\n";
  }
  for (const Json& tree : list) {
    out += std::format("  {:<28} pid {:>6}  {:>3} processes  CPU {:>5} %  memory {:>10} ({} %)\n",
                       text_of(tree, "image"), fixed(number(tree, "pid"), 0),
                       fixed(number(tree, "processes"), 0), fixed(number(tree, "cpu_percent")),
                       bytes(number(tree, "private_working_set")),
                       fixed(number(tree, "memory_percent")));
    out += "      " + trust_line(tree.value("trust", Json())) + "\n";
  }
  return out;
}

std::string volumes(const Json& volumes) {
  std::string out = "Volumes:\n";
  for (const Json& volume : volumes) {
    out += std::format("  {:<12} {} free of {} ({} %), {}{}\n", text_of(volume, "volume"),
                       bytes(number(volume, "free")), bytes(number(volume, "size")),
                       fixed(number(volume, "free_percent"), 0), fill(volume),
                       volume.value("system", false) ? ", Windows volume" : "");
  }
  return out;
}

std::string system(const Json& latest) {
  const Json& cpu = latest.value("cpu", Json::object());
  const Json& memory = latest.value("memory", Json::object());
  std::string out = std::format(
      "CPU: {} % of {} logical processors, queue {}, DPC {} %, interrupt {} %\n",
      fixed(number(cpu, "utility")), fixed(number(cpu, "logical_processors"), 0),
      fixed(number(cpu, "queue"), 0), fixed(number(cpu, "dpc")), fixed(number(cpu, "interrupt")));
  out += std::format(
      "Memory: {} available of {}, commit {} of {}, standby {}, compressed {}, hard faults {}/s\n",
      bytes(number(memory, "available")), bytes(number(memory, "total")),
      bytes(number(memory, "commit")), bytes(number(memory, "commit_limit")),
      bytes(number(memory, "standby")), bytes(number(memory, "compressed")),
      fixed(number(memory, "hard_faults"), 0));
  out += "Disks:\n";
  for (const Json& disk : latest.value("disks", Json::array())) {
    out += std::format(
        "  disk {:<3} read {}/s, write {}/s, {} IOPS, latency {}/{} ms, queue {}, idle {} %\n",
        text_of(disk, "disk"), bytes(number(disk, "read_bytes")),
        bytes(number(disk, "write_bytes")), fixed(number(disk, "iops"), 0),
        fixed(number(disk, "read_latency")), fixed(number(disk, "write_latency")),
        fixed(number(disk, "queue")), fixed(number(disk, "idle"), 0));
  }
  out += volumes(latest.value("volumes", Json::array()));
  const Json& gpu = latest.value("gpu", Json());
  for (const Json& adapter : gpu.is_object() ? gpu.value("adapters", Json::array()) : Json()) {
    out += std::format("GPU {}: {} %, {} dedicated, {} shared\n", text_of(adapter, "entity"),
                       fixed(number(adapter, "utilization"), 0),
                       bytes(number(adapter, "dedicated")), bytes(number(adapter, "shared")));
  }
  for (const Json& missing : latest.value("unavailable", Json::array())) {
    out += "Unavailable: " + missing.get<std::string>() + "\n";
  }
  return out;
}

std::string health(const Json& snapshot) {
  const Json& signals = snapshot.value("signals", Json::object());
  std::string out = std::format("MyClean health at {}\n", utc(snapshot, "time"));
  out += trees(snapshot);
  out += std::format("Signals: commit {} %, {} free {} %, disk queue {}\n",
                     fixed(number(signals, "commit_percent"), 0),
                     signals.value("system_volume", Json()).is_string()
                         ? text_of(signals, "system_volume")
                         : "Windows volume",
                     fixed(number(signals, "system_volume_free_percent"), 0),
                     fixed(number(signals, "disk_queue")));
  return out + system(snapshot);
}

std::string history(const Json& history) {
  const Json& points = history.value("points", Json::array());
  std::string out =
      std::format("{} of {} ({} resolution, {} points)\n", text_of(history, "metric"),
                  text_of(history, "entity"), text_of(history, "resolution"), points.size());
  for (const Json& point : points) {
    out += std::format("  {}  mean {}  min {}  max {}\n", utc(point, "time"),
                       fixed(number(point, "mean"), 2), fixed(number(point, "min"), 2),
                       fixed(number(point, "max"), 2));
  }
  return out;
}

std::string explain(const Json& process) {
  std::string out =
      std::format("{} (pid {}, parent {})\n", text_of(process, "image"),
                  fixed(number(process, "pid"), 0), fixed(number(process, "parent_pid"), 0));
  out += "  path: " + text_of(process, "path") + "\n";
  out += std::format(
      "  CPU {} %, memory {} ({} %), private bytes {}\n", fixed(number(process, "cpu_percent")),
      bytes(number(process, "private_working_set")), fixed(number(process, "memory_percent")),
      bytes(number(process, "private_bytes")));
  out += "  " + trust_line(process.value("trust", Json())) + "\n";
  const Json& kb = process.value("kb", Json());
  if (kb.is_object()) {
    out += std::format("  knowledge base: {} ({}); needed at startup: {}; safe to remove: {}\n",
                       text_of(kb, "description"), text_of(kb, "category"),
                       kb.value("needed_at_startup", false) ? "yes" : "no",
                       kb.value("safe_to_remove", false) ? "yes" : "no");
  } else {
    out += "  knowledge base: no entry\n";
  }
  const Json& tree = process.value("tree", Json());
  if (tree.is_object()) {
    out += std::format("  tree of {} (pid {}): {} processes, CPU {} %, memory {} %\n",
                       text_of(tree, "image"), fixed(number(tree, "pid"), 0),
                       fixed(number(tree, "processes"), 0), fixed(number(tree, "cpu_percent")),
                       fixed(number(tree, "memory_percent")));
    for (const Json& member : process.value("members", Json::array())) {
      out += std::format("    {:<28} pid {:>6}  CPU {:>5} %  memory {}\n", text_of(member, "image"),
                         fixed(number(member, "pid"), 0), fixed(number(member, "cpu_percent")),
                         bytes(number(member, "private_working_set")));
    }
  }
  return out;
}

std::string disks(const Json& health) {
  const Json& drives = health.value("drives", Json::array());
  std::string out = "Disk health:\n";
  if (drives.empty()) {
    out += "  no reading yet\n";
  }
  for (const Json& drive : drives) {
    std::string volumes;
    for (const Json& volume : drive.value("volumes", Json::array())) {
      volumes += (volumes.empty() ? "" : " ") + volume.get<std::string>();
    }
    out += std::format("  disk {:<3} {:<32} {:<5} {}{}\n", text_of(drive, "disk"),
                       text_of(drive, "model"), text_of(drive, "bus"), text_of(drive, "status"),
                       volumes.empty() ? "" : "  (" + volumes + ")");
    if (!drive.value("readable", false)) {
      out += "      " + text_of(drive, "unreadable_reason") + "\n";
    }
    for (const Json& reason : drive.value("reasons", Json::array())) {
      if (reason != drive.value("unreadable_reason", Json())) {
        out += "      " + reason.get<std::string>() + "\n";
      }
    }
    const bool has_limit = drive.value("temperature_limit_c", Json()).is_number();
    out += std::format(
        "      temperature {} (limit {}), wear {}, spare {}, media errors {}, reallocated {}, "
        "pending {}, power-on {}, written {}\n",
        optional_integer(drive, "temperature_c", " C"),
        has_limit ? optional_integer(drive, "temperature_limit_c", " C") : "not known",
        optional_integer(drive, "percent_used", " %"),
        optional_integer(drive, "available_spare", " %"),
        optional_integer(drive, "media_errors", ""),
        optional_integer(drive, "reallocated_sectors", ""),
        optional_integer(drive, "pending_sectors", ""),
        optional_integer(drive, "power_on_hours", " h"),
        bytes(number(drive, "data_written_bytes")));
    const Json& endurance = drive.value("endurance", Json::object());
    if (text_of(endurance, "state") == "projected") {
      out += std::format("      endurance: {} % of {} TBW used, about {} years left\n",
                         fixed(number(endurance, "tbw_used_percent"), 0),
                         fixed(number(endurance, "tbw_rated"), 0),
                         fixed(number(endurance, "years_left"), 0));
    } else if (!text_of(endurance, "note").empty()) {
      out += "      endurance: " + text_of(endurance, "note") + "\n";
    }
  }
  return out;
}

std::string alerts(const Json& list) {
  const Json& alerts = list.value("alerts", Json::array());
  std::string out = "Alerts:\n";
  if (alerts.empty()) {
    out += "  none\n";
  }
  for (const Json& alert : alerts) {
    std::string state = alert.value("active", false) ? "active" : "ended";
    if (alert.value("snoozed_until", Json()).is_number()) {
      state += ", snoozed until " + utc(alert, "snoozed_until");
    }
    out += std::format("  [{}] {} ({}, {}): {}x, last {}\n", text_of(alert, "severity"),
                       text_of(alert, "title"), text_of(alert, "entity"), state,
                       fixed(number(alert, "count"), 0), utc(alert, "last_at"));
    out += std::format("      rule {}; {}\n", text_of(alert, "rule"), text_of(alert, "detail"));
  }
  return out;
}

std::string freezes(const Json& summary) {
  const Json& groups = summary.value("groups", Json::array());
  if (groups.empty()) {
    return "No freeze over 250 ms has been recorded since the watchdog started.\n";
  }
  std::string out = "Copy/paste freezes by mechanism and process:\n";
  for (const Json& group : groups) {
    out += std::format("  {} {}: {} incidents, latest {}\n", text_of(group, "mechanism"),
                       group.value("process", Json()).is_string() ? text_of(group, "process")
                                                                  : std::string("(no process)"),
                       fixed(number(group, "count"), 0), utc(group, "latest"));
    for (const Json& incident : group.value("incidents", Json::array())) {
      out += std::format("    {} {} {} ms", utc(incident, "time"), text_of(incident, "chord"),
                         fixed(number(incident, "latency_ms"), 0));
      for (const Json& evidence : incident.value("evidence", Json::array())) {
        out += "; " + evidence.get<std::string>();
      }
      out += "\n";
    }
  }
  return out;
}

namespace {

// "cargo.exe (pid 100)" for an object with "image"/"root_image" and "root".
std::string tree_name(const Json& object, std::string_view image_key) {
  const Json& root = object.value("root", Json());
  if (!root.is_object()) {
    return text_of(object, image_key);
  }
  return std::format("{} (pid {})", text_of(object, image_key), fixed(number(root, "pid"), 0));
}

}  // namespace

std::string governor(const Json& status) {
  const Json& pressure = status.value("pressure", Json::object());
  const Json& metrics = pressure.value("metrics", Json());
  std::string reasons;
  for (const Json& reason : pressure.value("reasons", Json::array())) {
    reasons += (reasons.empty() ? "" : ", ") + reason.get<std::string>();
  }
  std::string out =
      std::format("Governor: {} mode, {}{}{}\n", text_of(status, "mode"),
                  text_of(pressure, "state"), reasons.empty() ? "" : " (" + reasons + ")",
                  pressure.value("clearing", false) ? ", clearing" : "");
  if (metrics.is_object()) {
    out += std::format("  CPU {} %, available {}, commit {} of {}, hard faults {}/s\n",
                       fixed(number(metrics, "cpu_percent")), bytes(number(metrics, "available")),
                       bytes(number(metrics, "commit")), bytes(number(metrics, "commit_limit")),
                       fixed(number(metrics, "hard_faults_per_second"), 0));
  }
  if (status.value("paused_until", Json()).is_number()) {
    out += "  paused until " + utc(status, "paused_until") + "\n";
  }
  const Json& game = status.value("game_mode", Json::object());
  out += std::format("Game mode: {} (override {}), build CPU cap {} %\n",
                     game.value("active", false) ? "on" : "off", text_of(game, "override"),
                     fixed(number(game, "build_cpu_percent"), 0));
  for (const Json& detected : game.value("games", Json::array())) {
    out += std::format("  game {} (pid {}): {}\n", text_of(detected, "image"),
                       fixed(number(detected, "pid"), 0), text_of(detected, "reason"));
  }
  const Json& jobs = status.value("jobs", Json::array());
  out += jobs.empty() ? "Jobs: none\n" : "Jobs:\n";
  for (const Json& job : jobs) {
    out += std::format("  {} {} {}: {} processes, CPU {} %, commit {} of {}{}\n",
                       text_of(job, "role"), text_of(job, "class"), tree_name(job, "root_image"),
                       fixed(number(job, "members"), 0), fixed(number(job, "cpu_cap_percent"), 0),
                       bytes(number(job, "commit_used")), bytes(number(job, "commit_cap")),
                       job.value("adopted", false) ? " (re-adopted)" : "");
  }
  for (const Json& tree : status.value("trees", Json::array())) {
    out += std::format("Step in effect: {} on {} ({})\n", text_of(tree, "step"),
                       tree_name(tree, "image"), text_of(tree, "class"));
  }
  for (const Json& notification : status.value("notifications", Json::array())) {
    out += std::format("Notification {}: {} on {}, {}\n", fixed(number(notification, "id"), 0),
                       text_of(notification, "step"), tree_name(notification, "image"),
                       text_of(notification, "reason"));
  }
  const Json& events = status.value("events", Json::array());
  out += events.empty() ? "Recent events: none\n" : "Recent events, newest first:\n";
  for (const Json& event : events) {
    const Json& step = event.value("step", Json());
    out += std::format("  {} {}{} {}: {}, {}{}\n", utc(event, "time"), text_of(event, "kind"),
                       step.is_string() ? " " + step.get<std::string>() : "",
                       tree_name(event, "image"), text_of(event, "reason"), text_of(event, "actor"),
                       text_of(event, "detail").empty() ? "" : " - " + text_of(event, "detail"));
  }
  return out;
}

namespace {

// How an area's space can be reclaimed, from its "reclaim" object.
std::string how(const Json& reclaim) {
  const std::string kind = text_of(reclaim, "kind");
  const std::string ref = text_of(reclaim, "ref");
  if (kind == "clean-rule") {
    return "clean rule " + ref;
  }
  if (kind == "handler") {
    return "cleanup handler " + ref;
  }
  if (kind == "relocate" || kind == "setting") {
    return kind + " " + ref;
  }
  if (kind == "manual-review") {
    return "your review";
  }
  return kind == "uninstall" ? "uninstall" : "needed as it is";
}

// The areas of a map by id.
class AreaIndex {
 public:
  explicit AreaIndex(const Json& map) {
    const auto found = map.find("areas");
    if (found == map.end() || !found->is_array()) {
      return;
    }
    for (const Json& area : *found) {
      areas_.emplace(text_of(area, "id"), &area);
    }
  }

  // "[category] label (how)" for an area id; empty for null or an unknown id.
  [[nodiscard]] std::string describe(const Json& id) const {
    const auto found = id.is_string() ? areas_.find(id.get<std::string>()) : areas_.end();
    if (found == areas_.end()) {
      return {};
    }
    const Json& area = *found->second;
    return std::format("[{}] {} ({})", text_of(area, "category"), text_of(area, "label"),
                       how(area.value("reclaim", Json::object())));
  }

 private:
  std::map<std::string, const Json*, std::less<>> areas_;
};

std::string leaf_of(const std::string& path) {
  const std::size_t separator = path.find_last_of("\\/");
  return separator == std::string::npos || separator + 1 == path.size()
             ? path
             : path.substr(separator + 1);
}

// NOLINTNEXTLINE(misc-no-recursion): the depth is bounded by the map's depth option
void tree_lines(const Json& node, int level, const AreaIndex& areas, std::string& out) {
  const std::string path = text_of(node, "path");
  std::string line = std::format("{:>12}  {}{}", bytes(number(node, "size")),
                                 std::string(static_cast<std::size_t>(level) * 2, ' '),
                                 level == 0 ? path : leaf_of(path));
  if (node.value("matched_here", false)) {
    line += "  " + areas.describe(node.value("area", Json()));
  } else if (!node.value("area", Json()).is_string() && number(node, "unexplained") > 0.0) {
    line += "  unexplained " + bytes(number(node, "unexplained"));
  }
  if (number(node, "apparent") > number(node, "size")) {
    line += "  (" + bytes(number(node, "apparent")) + " counting hard links)";
  }
  out += line + "\n";
  if (const auto children = node.find("children"); children != node.end()) {
    for (const Json& child : *children) {
      tree_lines(child, level + 1, areas, out);
    }
  }
}

std::string percent_of(std::optional<double> part, std::optional<double> whole) {
  return part && whole && *whole > 0.0 ? std::format("{:.0f} %", *part * 100.0 / *whole) : "n/a";
}

// The reclaimable areas by kind, with how and the note of each.
std::string reclaim_lines(const Json& map) {
  std::string out;
  out += std::format(
      "\nReclaimable: up to {} (each action previews the exact amount before it runs)\n",
      bytes(number(map, "reclaimable")));
  for (const std::string_view kind :
       {"setting", "relocate", "clean-rule", "handler", "uninstall"}) {
    for (const Json& area : map.value("areas", Json::array())) {
      const Json& reclaim = area.value("reclaim", Json::object());
      if (text_of(reclaim, "kind") != kind || number(area, "size").value_or(0.0) <= 0.0) {
        continue;
      }
      out += std::format("  {:>12}  {}: {}\n", bytes(number(area, "size")), how(reclaim),
                         text_of(area, "label"));
      if (!text_of(reclaim, "note").empty()) {
        out += std::string(16, ' ') + text_of(reclaim, "note") + "\n";
      }
    }
  }
  out += std::format("Needs your review: {}\n", bytes(number(map, "review")));
  return out;
}

// The used space the scan could not see and the folders it could not read.
std::string not_visible_lines(const Json& map, const AreaIndex& areas) {
  std::string out;
  out += "\nNot visible:\n";
  if (map.value("not_visible", Json()).is_number()) {
    out += std::format(
        "  {} of the used space was not seen by this scan. MyClean does not guess what it is;\n"
        "  it is made of the folders below that could not be read (restore points, other\n"
        "  users' profiles, protected system folders), NTFS metadata ($MFT, $LogFile) and\n"
        "  anything else a folder listing cannot see. Run as administrator to see more.\n",
        bytes(number(map, "not_visible")));
  } else {
    out += "  (the volume's used space is known only when a whole volume is mapped)\n";
  }
  const Json& unreadable = map.value("unreadable", Json::array());
  out += std::format("  {} folders could not be read{}\n", unreadable.size(),
                     unreadable.empty() ? "." : ":");
  constexpr std::size_t shown = 30;
  for (std::size_t index = 0; index < unreadable.size() && index < shown; ++index) {
    const Json& folder = unreadable[index];
    out += std::format("    {}  ({})  {}\n", text_of(folder, "path"), text_of(folder, "error"),
                       areas.describe(folder.value("area", Json())));
  }
  if (unreadable.size() > shown) {
    out += std::format("    ... and {} more (--json lists every one)\n", unreadable.size() - shown);
  }
  return out;
}

}  // namespace

std::string map(const Json& map) {
  const AreaIndex areas(map);
  const std::string minimum = bytes(number(map, "min_size"));
  std::string out = "Disk map of " + text_of(map, "root");
  if (const Json& volume = map.value("volume", Json()); volume.is_object()) {
    out += std::format(" (volume {}, {} used, {} free)", bytes(number(volume, "total")),
                       bytes(number(volume, "used")), bytes(number(volume, "free")));
  }
  out += map.value("cancelled", false) ? " - the scan stopped early\n" : "\n";
  out += std::format("  scanned     {:>12}", bytes(number(map, "scanned")));
  if (number(map, "apparent") > number(map, "scanned")) {
    out += std::format("  ({} counting every hard link)", bytes(number(map, "apparent")));
  }
  out += std::format("\n  explained   {:>12}  ({} of the scan)\n  unexplained {:>12}\n",
                     bytes(number(map, "explained")),
                     percent_of(number(map, "explained"), number(map, "scanned")),
                     bytes(number(map, "unexplained")));
  if (map.value("not_visible", Json()).is_number()) {
    out += std::format("  not visible {:>12}  (used space the scan could not see; see the end)\n",
                       bytes(number(map, "not_visible")));
  }

  out += std::format("\nBy category:\n  {:<18} {:>12} {:>14}\n", "category", "size", "reclaimable");
  for (const Json& category : map.value("categories", Json::array())) {
    out += std::format("  {:<18} {:>12} {:>14}\n", text_of(category, "category"),
                       bytes(number(category, "size")), bytes(number(category, "reclaimable")));
  }
  out += std::format("  {:<18} {:>12}\n", "(unexplained)", bytes(number(map, "unexplained")));

  out += reclaim_lines(map);

  out += std::format("\nLargest explained places (at least {}):\n", minimum);
  for (const Json& match : map.value("matches", Json::array())) {
    if (number(match, "size") < number(map, "min_size")) {
      break;
    }
    out += std::format("  {:>12}  {}  {}\n", bytes(number(match, "size")), text_of(match, "path"),
                       areas.describe(match.value("area", Json())));
  }

  out += std::format("\nTree (folders and files of at least {}, {} levels):\n", minimum,
                     fixed(number(map, "depth"), 0));
  if (const auto tree = map.find("tree"); tree != map.end()) {
    tree_lines(*tree, 0, areas, out);
  }

  out += std::format("\nUnexplained folders (at least {} that no area explains):\n", minimum);
  const Json& unexplained = map.value("unexplained_folders", Json::array());
  if (unexplained.empty()) {
    out += "  none\n";
  }
  for (const Json& folder : unexplained) {
    out += std::format("  {:>12}  {}\n", bytes(number(folder, "unexplained")),
                       text_of(folder, "path"));
  }

  out += not_visible_lines(map, areas);
  return out;
}

}  // namespace myclean::cli::text
