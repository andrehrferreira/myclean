#include "core/report/snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/disk_health.hpp"
#include "core/ipc/plans.hpp"
#include "core/ipc/resident.hpp"
#include "core/ipc/security.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/report/month.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

namespace myclean::report {
namespace {

using Json = nlohmann::json;

// Items kept per list, so a snapshot fits one pipe message with room to spare.
constexpr std::size_t max_items = 1000;
constexpr std::size_t top_programs = 15;
constexpr std::size_t top_talkers = 10;
constexpr int free_space_points = 30;

Json not_collected(std::string reason) {
  return Json{{"status", "not_collected"}, {"reason", std::move(reason)}};
}

Json collected_with(Json data) {
  data["status"] = "collected";
  return data;
}

std::string text(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

Json member(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() ? *found : Json(nullptr);
}

double number(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_number() ? found->get<double>() : 0.0;
}

std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return value;
}

std::int64_t unix_seconds(WallTime time) {
  return store::to_unix_seconds(time);
}

// The answers of the read methods, each asked once per snapshot.
class Answers {
 public:
  explicit Answers(const Call& call) : call_(&call) {}

  const Result<Json>& get(std::string_view method, const Json& params = Json::object()) {
    const std::string key = std::string(method) + params.dump();
    auto found = answers_.find(key);
    if (found == answers_.end()) {
      Result<Json> answer = (*call_)(method, params);
      if (!answer) {
        answer = fail(answer.error().code, std::string(method) + ": " + answer.error().message);
      }
      found = answers_.emplace(key, std::move(answer)).first;
    }
    return found->second;
  }

  void put(std::string_view method, const Json& params, Result<Json> answer) {
    answers_.insert_or_assign(std::string(method) + params.dump(), std::move(answer));
  }

 private:
  const Call* call_;
  std::map<std::string, Result<Json>> answers_;
};

// Runs `build`; an answer of an unexpected shape makes the category not collected.
Json guarded(const std::function<Json()>& build) {
  try {
    return build();
  } catch (const Json::exception& error) {
    return not_collected(std::string("unexpected answer: ") + error.what());
  }
}

// The storage analyses of prompt 3, started together, then asked again until each is ready.
struct Analysis {
  std::string_view method;
  Json params;
};

void run_analyses(const Probes& probes, Answers& answers, const std::vector<Analysis>& analyses) {
  std::vector<Result<Json>> results;
  results.reserve(analyses.size());
  for (const Analysis& analysis : analyses) {
    Json refresh = analysis.params;
    refresh["refresh"] = true;
    results.push_back(answers.get(analysis.method, refresh));
  }
  bool stopped = false;
  for (std::size_t index = 0; index < analyses.size(); ++index) {
    Result<Json>& result = results[index];
    int polls = 0;
    while (result && text(*result, "state") == "scanning") {
      if (stopped || polls >= probes.max_polls ||
          (probes.wait && !probes.wait(probes.poll_interval))) {
        stopped = stopped || polls < probes.max_polls;
        result = fail(Errc::invalid_state,
                      std::string(analyses[index].method) +
                          (stopped ? ": the monthly job stopped first"
                                   : ": still running after " + std::to_string(polls) + " polls"));
        break;
      }
      ++polls;
      result = probes.call(analyses[index].method, analyses[index].params);
      if (!result) {
        result = fail(result.error().code,
                      std::string(analyses[index].method) + ": " + result.error().message);
      }
    }
    if (result && text(*result, "state") == "failed") {
      result =
          fail(Errc::io_error, std::string(analyses[index].method) + ": " + text(*result, "error"));
    }
    answers.put(analyses[index].method, analyses[index].params, std::move(result));
  }
}

Json startup_category(const Result<Json>& answer, bool services) {
  if (!answer) {
    return not_collected(answer.error().message);
  }
  Json items = Json::array();
  for (const Json& item : answer->at("items")) {
    if ((text(item, "source") == "service") != services || items.size() >= max_items) {
      continue;
    }
    Json entry{{"key", text(item, "key")},
               {"name", text(item, "name")},
               {"command", text(item, "command")}};
    if (services) {
      entry["display_name"] = text(item, "display_name");
      entry["signer"] = member(item, "signer");
    } else {
      entry["source"] = text(item, "source");
      entry["scope"] = text(item, "scope");
      entry["enabled"] = member(item, "enabled");
    }
    items.push_back(std::move(entry));
  }
  return collected_with(Json{{"items", std::move(items)}});
}

Json extensions_category(const Result<Json>& answer) {
  if (!answer) {
    return not_collected(answer.error().message);
  }
  Json items = Json::array();
  for (const Json& extension : answer->at("extensions")) {
    if (items.size() >= max_items) {
      break;
    }
    items.push_back(Json{{"key", text(extension, "browser") + "/" + text(extension, "profile") +
                                     "/" + text(extension, "id")},
                         {"browser", text(extension, "browser")},
                         {"profile", text(extension, "profile")},
                         {"id", text(extension, "id")},
                         {"name", text(extension, "name")},
                         {"version", text(extension, "version")},
                         {"origin", text(extension, "origin")},
                         {"enabled", member(extension, "enabled")},
                         {"flag", member(extension, "flag")}});
  }
  return collected_with(Json{{"items", std::move(items)}});
}

Json ports_category(const Result<Json>& answer) {
  if (!answer) {
    return not_collected(answer.error().message);
  }
  std::map<std::string, Json> ports;
  for (const Json& process : answer->at("processes")) {
    const std::string image = text(process, "image");
    for (const Json& listener : process.value("listening", Json::array())) {
      const std::string protocol = text(listener, "protocol");
      const Json port = member(listener, "port");
      const std::string key = protocol + "/" + port.dump() + "/" + lower(image);
      ports.try_emplace(key, Json{{"key", key},
                                  {"protocol", protocol},
                                  {"port", port},
                                  {"image", image},
                                  {"pid", member(process, "pid")}});
    }
  }
  Json items = Json::array();
  for (auto& [key, port] : ports) {
    if (items.size() < max_items) {
      items.push_back(std::move(port));
    }
  }
  return collected_with(Json{{"items", std::move(items)}});
}

// Adds the image of `trust` when its signature is missing or invalid.
void add_unsigned(std::map<std::string, Json>& found, const Json& trust, std::string_view seen_as) {
  if (!trust.is_object()) {
    return;
  }
  const std::string status = text(trust, "status");
  const std::string path = text(trust, "path");
  if ((status != ipc::trust_unsigned && status != ipc::trust_invalid) || path.empty()) {
    return;
  }
  const std::string key = lower(path);
  auto [entry, added] = found.try_emplace(
      key, Json{{"key", key}, {"path", path}, {"status", status}, {"seen_as", Json::array()}});
  Json& seen = entry->second.at("seen_as");
  if (std::ranges::find(seen, Json(seen_as)) == seen.end()) {
    seen.push_back(seen_as);
  }
}

Json unsigned_category(const Result<Json>& processes, const Result<Json>& scan,
                       const Result<Json>& network) {
  if (!processes && !scan && !network) {
    return not_collected(processes.error().message + "; " + scan.error().message + "; " +
                         network.error().message);
  }
  std::map<std::string, Json> found;
  Json problems = Json::array();
  if (processes) {
    for (const Json& tree : processes->at("trees")) {
      add_unsigned(found, member(tree, "trust"), "process");
    }
  } else {
    problems.push_back(processes.error().message);
  }
  if (scan) {
    for (const Json& item : scan->at("items")) {
      add_unsigned(found, member(item, "trust"), "persistence");
    }
  } else {
    problems.push_back(scan.error().message);
  }
  if (network) {
    for (const Json& process : network->at("processes")) {
      add_unsigned(found, member(process, "trust"), "network");
    }
  } else {
    problems.push_back(network.error().message);
  }
  Json items = Json::array();
  for (auto& [key, item] : found) {
    if (items.size() < max_items) {
      items.push_back(std::move(item));
    }
  }
  return collected_with(Json{{"items", std::move(items)}, {"problems", std::move(problems)}});
}

Json top_folders_category(const Result<Json>& answer, const std::string& volume) {
  if (!answer) {
    return not_collected(answer.error().message);
  }
  Json folders = Json::array();
  for (const Json& folder : answer->at("folders")) {
    folders.push_back(Json{{"path", text(folder, "path")},
                           {"size_on_disk", member(folder, "size_on_disk")},
                           {"files", member(folder, "files")}});
  }
  Json files = Json::array();
  for (const Json& file : answer->at("files")) {
    files.push_back(
        Json{{"path", text(file, "path")}, {"size_on_disk", member(file, "size_on_disk")}});
  }
  return collected_with(
      Json{{"volume", volume}, {"folders", std::move(folders)}, {"files", std::move(files)}});
}

// {"count", "bytes"} of one cleanup analysis, or {"error"}.
Json candidates_of(const Result<Json>& answer, std::string_view list, std::string_view size) {
  if (!answer) {
    return Json{{"error", answer.error().message}};
  }
  std::int64_t count = 0;
  double bytes = 0.0;
  if (list == "groups") {
    for (const Json& group : answer->at("groups")) {
      count += static_cast<std::int64_t>(group.at("files").size()) - 1;
    }
    bytes = number(*answer, "reclaimable");
  } else {
    for (const Json& file : answer->at(list)) {
      ++count;
      bytes += number(file, size);
    }
  }
  return Json{{"count", count}, {"bytes", static_cast<std::int64_t>(bytes)}};
}

Json drives_category(const Result<Json>& answer) {
  if (!answer) {
    return not_collected(answer.error().message);
  }
  if (answer->at("drives").empty()) {
    return not_collected("disks.health: no disk health reading yet");
  }
  Json drives = Json::array();
  for (const Json& drive : answer->at("drives")) {
    Json entry{{"key", text(drive, "key")},
               {"model", text(drive, "model")},
               {"status", text(drive, "status")},
               {"reasons", drive.value("reasons", Json::array())}};
    for (const std::string_view counter :
         {"temperature_c", "percent_used", "available_spare", "media_errors", "reallocated_sectors",
          "pending_sectors", "power_on_hours", "data_written_bytes"}) {
      entry[std::string(counter)] = member(drive, counter);
    }
    drives.push_back(std::move(entry));
  }
  return collected_with(Json{{"drives", std::move(drives)}});
}

Json free_space_category(const Result<Json>& volumes, const Result<Json>& history,
                         const std::string& volume) {
  if (!volumes) {
    return not_collected(volumes.error().message);
  }
  if (!history) {
    return not_collected(history.error().message);
  }
  Json size = nullptr;
  Json free = nullptr;
  for (const Json& entry : volumes->at("volumes")) {
    if (text(entry, "volume") == volume) {
      size = member(entry, "size");
      free = member(entry, "free");
    }
  }
  Json points = Json::array();
  for (const Json& point : history->at("points")) {
    points.push_back(Json{{"time", member(point, "time")},
                          {"free", static_cast<std::int64_t>(number(point, "mean"))}});
  }
  if (points.empty()) {
    return not_collected("metrics.query: no free-space history of " + volume + " yet");
  }
  const std::int64_t change =
      points.back().at("free").get<std::int64_t>() - points.front().at("free").get<std::int64_t>();
  return collected_with(Json{{"volume", volume},
                             {"size", std::move(size)},
                             {"free", std::move(free)},
                             {"points", std::move(points)},
                             {"change", change}});
}

// The one point of a 30-day query merged into a single bucket; null without history.
Json aggregate(const Result<Json>& answer, std::string_view low_or_high) {
  if (!answer || answer->at("points").empty()) {
    return nullptr;
  }
  const Json& point = answer->at("points").front();
  return Json{{"mean", member(point, "mean")},
              {std::string(low_or_high), member(point, low_or_high)}};
}

Json health_category(const Result<Json>& costs, const Result<Json>& cpu, const Result<Json>& memory,
                     const Result<Json>& latest) {
  if (!costs) {
    return not_collected(costs.error().message);
  }
  double processors = 1.0;
  if (latest) {
    processors = std::max(1.0, number(latest->value("cpu", Json::object()), "logical_processors"));
  }
  Json programs = Json::array();
  for (const Json& program : costs->at("programs")) {
    if (programs.size() >= top_programs) {
      break;
    }
    const double hours = number(program, "hours_running");
    const double percent =
        hours > 0.0 ? number(program, "cpu_seconds") / (hours * 3600.0 * processors) * 100.0 : 0.0;
    programs.push_back(Json{{"image", text(program, "image")},
                            {"hours_running", hours},
                            {"cpu_percent", percent},
                            {"ram_average", member(program, "ram_average")},
                            {"ram_peak", member(program, "ram_peak")}});
  }
  return collected_with(Json{{"days", health_days.count()},
                             {"cpu", aggregate(cpu, "max")},
                             {"memory_available", aggregate(memory, "min")},
                             {"programs", std::move(programs)}});
}

Json security_category(const Result<Json>& scan, const Result<Json>& extensions,
                       const Result<Json>& network) {
  if (!scan) {
    return not_collected(scan.error().message);
  }
  Json suspicious = Json::array();
  Json dangerous = Json::array();
  for (const Json& item : scan->at("items")) {
    const std::string status = text(item, "status");
    Json entry{{"name", text(item, "name")},
               {"source", text(item, "source")},
               {"summary", text(item, "summary")}};
    if (status == "known_dangerous") {
      dangerous.push_back(std::move(entry));
    } else if (status == "suspicious" && suspicious.size() < max_items) {
      suspicious.push_back(std::move(entry));
    }
  }
  Json result{{"counts", scan->value("counts", Json::object())},
              {"suspicious", std::move(suspicious)},
              {"known_dangerous", std::move(dangerous)},
              {"defender_detections",
               scan->value("defender", Json::object()).value("detections", Json::array()).size()},
              {"flagged_extensions", nullptr},
              {"background_talkers", nullptr}};
  if (extensions) {
    result["flagged_extensions"] = std::ranges::count_if(
        extensions->at("extensions"),
        [](const Json& extension) { return member(extension, "flag").is_string(); });
  }
  if (network) {
    std::vector<Json> talkers;
    for (const Json& process : network->at("processes")) {
      if (member(process, "visible_window") == Json(false)) {
        talkers.push_back(Json{{"image", text(process, "image")},
                               {"sent", member(process, "sent")},
                               {"received", member(process, "received")}});
      }
    }
    std::ranges::sort(talkers, [](const Json& left, const Json& right) {
      return number(left, "sent") + number(left, "received") >
             number(right, "sent") + number(right, "received");
    });
    talkers.resize(std::min(talkers.size(), top_talkers));
    result["background_talkers"] = talkers;
  }
  return collected_with(std::move(result));
}

// Runs `query` on the store; the category is not collected when the store is missing or fails.
Json from_store(store::Store* store, const std::function<Result<Json>(store::Database&)>& query) {
  if (store == nullptr) {
    return not_collected("the history store is not available");
  }
  Result<Json> data = fail(Errc::io_error, "not read");
  const Result<void> read = store->read([&](store::Database& db) -> Result<void> {
    data = query(db);
    return {};
  });
  if (!read) {
    return not_collected(read.error().message);
  }
  return data ? collected_with(std::move(*data)) : not_collected(data.error().message);
}

Result<Json> governor_counts(store::Database& db, WallTime from, WallTime to) {
  Result<store::Statement> query = db.prepare(
      "SELECT COALESCE(step, kind), detail = 'observe', COUNT(*) FROM governor_event "
      "WHERE time >= ?1 AND time < ?2 AND kind IN ('apply_step', 'kill') "
      "GROUP BY COALESCE(step, kind), detail = 'observe'");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, unix_seconds(from)).bind(2, unix_seconds(to));
  std::int64_t applied = 0;
  std::int64_t observed = 0;
  Json by_step = Json::object();
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    const std::int64_t count = query->column_int(2);
    (query->column_int(1) != 0 ? observed : applied) += count;
    const std::string step = query->column_text(0);
    by_step[step] = by_step.value(step, std::int64_t{0}) + count;
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return Json{{"interventions", applied + observed},
              {"applied", applied},
              {"recorded_only", observed},
              {"by_step", std::move(by_step)}};
}

Result<Json> freeze_counts(store::Database& db, WallTime from, WallTime to) {
  Result<store::Statement> query = db.prepare(
      "SELECT COUNT(*), MAX(latency_ms) FROM input_incident WHERE time >= ?1 AND time < ?2");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, unix_seconds(from)).bind(2, unix_seconds(to));
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  return Json{{"incidents", query->column_int(0)},
              {"longest_ms", query->is_null(1) ? Json(nullptr) : Json(query->column_int(1))}};
}

// The bytes the automatic cleaning deleted: its plans run with the actor "scheduler", and each
// item it removed is an "apply" entry with "freed=<bytes>".
Result<Json> freed_counts(store::Database& db, WallTime from, WallTime to) {
  Result<store::Statement> query = db.prepare(
      "SELECT metrics FROM action_log WHERE time >= ?1 AND time < ?2 AND event = 'apply' AND "
      "actor = 'scheduler' AND outcome IN ('removed', 'partial') AND item_id IS NOT NULL");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, unix_seconds(from)).bind(2, unix_seconds(to));
  constexpr std::string_view prefix = "freed=";
  std::uint64_t bytes = 0;
  std::int64_t items = 0;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    const std::string metrics = query->column_text(0);
    const std::size_t at = metrics.find(prefix);
    if (at == std::string::npos) {
      continue;
    }
    std::uint64_t value = 0;
    for (std::size_t index = at + prefix.size();
         index < metrics.size() && metrics[index] >= '0' && metrics[index] <= '9'; ++index) {
      value = (value * 10U) + static_cast<std::uint64_t>(metrics[index] - '0');
    }
    bytes += value;
    ++items;
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return Json{{"bytes", bytes}, {"items", items}};
}

// The volume Windows runs from ("C:") as volumes.list names it; "C:" when none is marked.
std::string system_volume(const Result<Json>& volumes) {
  if (volumes && volumes->contains("volumes") && volumes->at("volumes").is_array()) {
    for (const Json& volume : volumes->at("volumes")) {
      if (volume.value("system", false) && volume.contains("volume") &&
          volume.at("volume").is_string()) {
        return volume.at("volume").get<std::string>();
      }
    }
  }
  return "C:";
}

}  // namespace

Json category(const Json& snapshot, std::string_view name) {
  const Json categories_of = snapshot.value("categories", Json::object());
  const auto found = categories_of.find(name);
  return found != categories_of.end() && found->is_object() ? *found
                                                            : not_collected("not in the snapshot");
}

bool collected(const Json& category) {
  return category.is_object() && category.value("status", "") == "collected";
}

Json take_snapshot(const Probes& probes, WallTime now, WallTime period_start,
                   const std::chrono::time_zone& zone) {
  Answers answers(probes.call);
  const std::int64_t to = unix_seconds(now);
  const std::int64_t month_ago = unix_seconds(now - health_days);
  const Result<Json>& volumes = answers.get(ipc::volumes_list_method);
  const std::string volume = system_volume(volumes);
  const std::string token = ipc::entity_token(volume);

  run_analyses(probes, answers,
               {{.method = ipc::space_top_method, .params = Json{{"path", token}, {"n", 20}}},
                {.method = ipc::space_old_downloads_method, .params = Json::object()},
                {.method = ipc::space_duplicates_method, .params = Json{{"mode", "photos"}}},
                {.method = ipc::space_used_installers_method, .params = Json::object()}});

  const Result<Json>& startup = answers.get(ipc::startup_list_method);
  const Result<Json>& extensions = answers.get(ipc::security_extensions_method);
  const Result<Json>& network = answers.get(ipc::security_network_method, Json{{"hours", 24.0}});
  const Result<Json>& scan = answers.get(ipc::security_scan_method);
  const Result<Json>& processes =
      answers.get(ipc::processes_top_method, Json{{"limit", ipc::max_top_trees}});

  Json result = Json::object();
  result["startup_items"] = guarded([&] { return startup_category(startup, false); });
  result["browser_extensions"] = guarded([&] { return extensions_category(extensions); });
  result["services"] = guarded([&] { return startup_category(startup, true); });
  result["listening_ports"] = guarded([&] { return ports_category(network); });
  result["unsigned_binaries"] =
      guarded([&] { return unsigned_category(processes, scan, network); });
  result["top_folders"] = guarded([&] {
    return top_folders_category(
        answers.get(ipc::space_top_method, Json{{"path", token}, {"n", 20}}), volume);
  });
  result["cleanup_candidates"] = guarded([&] {
    return collected_with(Json{
        {"old_downloads",
         candidates_of(answers.get(ipc::space_old_downloads_method), "files", "size_on_disk")},
        {"duplicate_photos",
         candidates_of(answers.get(ipc::space_duplicates_method, Json{{"mode", "photos"}}),
                       "groups", "")},
        {"used_installers",
         candidates_of(answers.get(ipc::space_used_installers_method), "used", "size_on_disk")}});
  });
  result["disk_health"] =
      guarded([&] { return drives_category(answers.get(ipc::disks_health_method)); });
  result["free_space"] = guarded([&] {
    return free_space_category(
        volumes,
        answers.get(ipc::metrics_query_method, Json{{"metric", "volume.free"},
                                                    {"entity", token},
                                                    {"from", month_ago},
                                                    {"to", to},
                                                    {"max_points", free_space_points}}),
        volume);
  });
  const auto whole_month = [&](std::string_view metric) {
    return Json{{"metric", metric},
                {"entity", "system"},
                {"from", month_ago},
                {"to", to},
                {"max_points", 1}};
  };
  result["health"] = guarded([&] {
    return health_category(answers.get(ipc::resident_costs_method),
                           answers.get(ipc::metrics_query_method, whole_month("cpu.utility")),
                           answers.get(ipc::metrics_query_method, whole_month("memory.available")),
                           answers.get(ipc::metrics_latest_method));
  });
  result["security"] = guarded([&] { return security_category(scan, extensions, network); });
  result["governor"] = from_store(
      probes.store, [&](store::Database& db) { return governor_counts(db, period_start, now); });
  result["freezes"] = from_store(
      probes.store, [&](store::Database& db) { return freeze_counts(db, period_start, now); });
  result["space_freed"] = from_store(
      probes.store, [&](store::Database& db) { return freed_counts(db, period_start, now); });

  return Json{{"month", month_text(month_of(now, zone))},
              {"taken_at", to},
              {"period", {{"from", unix_seconds(period_start)}, {"to", to}}},
              {"categories", std::move(result)}};
}

}  // namespace myclean::report
