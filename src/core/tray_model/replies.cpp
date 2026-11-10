#include "core/tray_model/replies.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/alerts.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;

// Adds every number of `object` as series "<group>.<name>" of `entity`, except `skip`.
void add_numbers(MetricsSnapshot& metrics, const Json& object, std::string_view group,
                 const std::string& entity, WallTime time, std::string_view skip = {}) {
  for (const auto& [name, value] : object.items()) {
    if (value.is_number() && name != skip) {
      metrics.set(SeriesKey{.metric = std::string(group) + "." + name, .entity = entity},
                  Sample{.time = time, .value = value.get<double>()});
    }
  }
}

FillState parse_fill_state(const std::string& state) {
  if (state == "filling") {
    return FillState::filling;
  }
  if (state == "not_filling") {
    return FillState::not_filling;
  }
  return FillState::insufficient_data;
}

// `object[name]` when it is not null; throws when the member is missing.
template <typename T>
std::optional<T> nullable(const Json& object, const char* name) {
  const Json& value = object.at(name);
  return value.is_null() ? std::nullopt : std::optional<T>(value.get<T>());
}

std::optional<WallTime> nullable_time(const Json& object, const char* name) {
  const std::optional<std::int64_t> seconds = nullable<std::int64_t>(object, name);
  return seconds ? std::optional(WallTime{std::chrono::seconds{*seconds}}) : std::nullopt;
}

AlertSeverity parse_severity(const std::string& text) {
  for (const AlertSeverity severity :
       {AlertSeverity::info, AlertSeverity::warning, AlertSeverity::critical}) {
    if (text == to_string(severity)) {
      return severity;
    }
  }
  throw std::invalid_argument("unknown severity \"" + text + "\"");
}

DriveStatus parse_drive_status(const std::string& text) {
  for (const DriveStatus status :
       {DriveStatus::unknown, DriveStatus::good, DriveStatus::warning, DriveStatus::failing}) {
    if (text == to_string(status)) {
      return status;
    }
  }
  throw std::invalid_argument("unknown drive status \"" + text + "\"");
}

std::optional<TrustRow> parse_trust(const Json& trust) {
  if (trust.is_null()) {
    return std::nullopt;
  }
  TrustRow row;
  row.status = trust.at("status").get<std::string>();
  if (row.status != ipc::trust_signed && row.status != ipc::trust_unsigned &&
      row.status != ipc::trust_invalid) {
    throw std::invalid_argument("unknown trust status \"" + row.status + "\"");
  }
  row.signer = nullable<std::string>(trust, "signer");
  row.path = trust.at("path").get<std::string>();
  row.user_writable = trust.at("path_class").get<std::string>() == ipc::path_user_writable;
  row.unrecognized = trust.at("unrecognized").get<bool>();
  row.reasons = trust.at("reasons").get<std::vector<std::string>>();
  row.description = nullable<std::string>(trust, "description");
  row.category = nullable<std::string>(trust, "category");
  return row;
}

ProcessRow parse_member(const Json& member) {
  return ProcessRow{
      .pid = member.at("pid").get<std::uint32_t>(),
      .image = member.at("image").get<std::string>(),
      .cpu_percent = member.at("cpu_percent").get<double>(),
      .private_working_set = Bytes{member.at("private_working_set").get<std::uint64_t>()},
      .private_bytes = Bytes{member.at("private_bytes").get<std::uint64_t>()}};
}

VolumeRow parse_volume(const Json& volume) {
  VolumeRow row;
  row.name = volume.at("volume").get<std::string>();
  row.size = Bytes{volume.at("size").get<std::uint64_t>()};
  row.free = Bytes{volume.at("free").get<std::uint64_t>()};
  row.system = volume.value("system", false);
  for (const Json& disk : volume.at("disks")) {
    row.disks.push_back(std::to_string(disk.get<std::uint32_t>()));
  }
  const Json& fill = volume.at("fill");
  row.fill = parse_fill_state(fill.at("state").get<std::string>());
  const Json& days = fill.at("days_until_full");
  if (row.fill == FillState::filling && days.is_number()) {
    row.days_until_full = days.get<double>();
  }
  return row;
}

}  // namespace

std::int64_t unix_seconds(WallTime time) {
  return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

WallTime from_unix_seconds(std::int64_t seconds) {
  return WallTime{std::chrono::seconds{seconds}};
}

Result<LatestReply> parse_metrics_latest(const Json& reply) {
  try {
    LatestReply latest;
    latest.time = from_unix_seconds(reply.at("time").get<std::int64_t>());
    const std::string system(telemetry::metric::system);
    const Json& cpu = reply.at("cpu");
    latest.logical_processors = cpu.value("logical_processors", 0U);
    add_numbers(latest.metrics, cpu, "cpu", system, latest.time, "logical_processors");
    const auto processors = cpu.find("processors");
    if (processors != cpu.end()) {
      for (const Json& processor : *processors) {
        std::string entity = processor.at("entity").get<std::string>();
        add_numbers(latest.metrics, processor, "cpu", entity, latest.time);
        latest.processors.push_back(std::move(entity));
      }
    }
    add_numbers(latest.metrics, reply.at("memory"), "memory", system, latest.time);
    add_numbers(latest.metrics, reply.at("process"), "process", system, latest.time);
    for (const Json& disk : reply.at("disks")) {
      add_numbers(latest.metrics, disk, "disk", disk.at("disk").get<std::string>(), latest.time);
    }
    const Json& gpu = reply.at("gpu");
    if (!gpu.is_null()) {
      for (const Json& adapter : gpu.at("adapters")) {
        const std::string entity = adapter.at("adapter").get<std::string>();
        add_numbers(latest.metrics, adapter, "gpu", entity, latest.time);
        const auto engines = adapter.find("engines");
        if (engines != adapter.end()) {
          for (const auto& [engine, value] : engines->items()) {
            std::string engine_entity = entity;
            engine_entity.append("/").append(engine);
            latest.metrics.set(SeriesKey{.metric = std::string(telemetry::metric::gpu_engine),
                                         .entity = std::move(engine_entity)},
                               Sample{.time = latest.time, .value = value.get<double>()});
          }
        }
      }
    }
    for (const Json& volume : reply.at("volumes")) {
      latest.volumes.push_back(parse_volume(volume));
    }
    latest.unavailable = reply.at("unavailable").get<std::vector<std::string>>();
    return latest;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("metrics.latest: ") + error.what());
  }
}

Json metrics_query_params(std::string_view metric, std::string_view entity, std::int64_t from,
                          std::int64_t to, std::optional<int> max_points) {
  Json params{
      {"metric", metric}, {"entity", ipc::entity_token(entity)}, {"from", from}, {"to", to}};
  if (max_points) {
    params["max_points"] = *max_points;
  }
  return params;
}

Result<Series> parse_metrics_query(const Json& reply) {
  try {
    Series series{.key = SeriesKey{.metric = reply.at("metric").get<std::string>(),
                                   .entity = reply.at("entity").get<std::string>()},
                  .samples = {}};
    for (const Json& point : reply.at("points")) {
      series.samples.push_back(
          Sample{.time = from_unix_seconds(point.at("time").get<std::int64_t>()),
                 .value = point.at("mean").get<double>()});
    }
    return series;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("metrics.query: ") + error.what());
  }
}

Json processes_top_params(ProcessSort sort, int limit) {
  return Json{{"sort", sort == ProcessSort::memory ? "memory" : "cpu"}, {"limit", limit}};
}

Result<std::vector<ProcessTreeRow>> parse_processes_top(const Json& reply) {
  try {
    std::vector<ProcessTreeRow> trees;
    for (const Json& tree : reply.at("trees")) {
      ProcessTreeRow row;
      row.cpu_percent = tree.at("cpu_percent").get<double>();
      row.private_working_set = Bytes{tree.at("private_working_set").get<std::uint64_t>()};
      row.private_bytes = Bytes{tree.at("private_bytes").get<std::uint64_t>()};
      row.working_set = Bytes{tree.at("working_set").get<std::uint64_t>()};
      for (const Json& member : tree.at("members")) {
        row.members.push_back(parse_member(member));
      }
      if (row.members.empty()) {
        return fail(Errc::invalid_data, "processes.top: a tree without members");
      }
      row.trust = parse_trust(tree.at("trust"));
      trees.push_back(std::move(row));
    }
    return trees;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("processes.top: ") + error.what());
  }
}

Json processes_explain_params(std::uint32_t pid) {
  return Json{{"pid", pid}};
}

Result<Explanation> parse_processes_explain(const Json& reply) {
  try {
    Explanation explanation;
    explanation.pid = reply.at("pid").get<std::uint32_t>();
    explanation.image = reply.at("image").get<std::string>();
    explanation.path = nullable<std::string>(reply, "path");
    explanation.cpu_percent = reply.at("cpu_percent").get<double>();
    explanation.memory_percent = reply.at("memory_percent").get<double>();
    if (const Json& tree = reply.at("tree"); !tree.is_null()) {
      explanation.tree_pid = tree.at("pid").get<std::uint32_t>();
      explanation.tree_image = tree.at("image").get<std::string>();
    }
    for (const Json& member : reply.at("members")) {
      explanation.members.push_back(parse_member(member));
    }
    explanation.trust = parse_trust(reply.at("trust"));
    const Json& kb = reply.at("kb");
    if (!kb.is_null()) {
      explanation.kb = KbRow{.description = kb.at("description").get<std::string>(),
                             .category = kb.at("category").get<std::string>(),
                             .needed_at_startup = kb.at("needed_at_startup").get<bool>(),
                             .safe_to_remove = kb.at("safe_to_remove").get<bool>()};
    }
    return explanation;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("processes.explain: ") + error.what());
  }
}

Result<std::vector<DriveHealthRow>> parse_disks_health(const Json& reply) {
  try {
    std::vector<DriveHealthRow> drives;
    for (const Json& drive : reply.at("drives")) {
      DriveHealthRow row;
      row.key = drive.at("key").get<std::string>();
      row.disk = drive.at("disk").get<std::string>();
      row.model = drive.at("model").get<std::string>();
      row.bus = drive.at("bus").get<std::string>();
      row.source = drive.at("source").get<std::string>();
      row.volumes = drive.at("volumes").get<std::vector<std::string>>();
      row.status = parse_drive_status(drive.at("status").get<std::string>());
      row.reasons = drive.at("reasons").get<std::vector<std::string>>();
      row.readable = drive.at("readable").get<bool>();
      row.unreadable_reason = nullable<std::string>(drive, "unreadable_reason");
      row.temperature_c = nullable<std::int64_t>(drive, "temperature_c");
      row.temperature_limit_c = nullable<std::int64_t>(drive, "temperature_limit_c");
      row.percent_used = nullable<std::int64_t>(drive, "percent_used");
      row.available_spare = nullable<std::int64_t>(drive, "available_spare");
      row.media_errors = nullable<std::int64_t>(drive, "media_errors");
      row.reallocated_sectors = nullable<std::int64_t>(drive, "reallocated_sectors");
      row.pending_sectors = nullable<std::int64_t>(drive, "pending_sectors");
      row.power_on_hours = nullable<std::int64_t>(drive, "power_on_hours");
      row.data_written_bytes = nullable<std::int64_t>(drive, "data_written_bytes");
      const Json& endurance = drive.at("endurance");
      row.endurance_state = endurance.at("state").get<std::string>();
      row.years_left = nullable<double>(endurance, "years_left");
      row.tbw_rated = nullable<double>(endurance, "tbw_rated");
      row.tbw_used_percent = nullable<double>(endurance, "tbw_used_percent");
      row.endurance_note = endurance.at("note").get<std::string>();
      drives.push_back(std::move(row));
    }
    return drives;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("disks.health: ") + error.what());
  }
}

Result<AlertSummary> parse_alerts_badge(const Json& reply) {
  try {
    AlertSummary summary;
    summary.active = reply.at("count").get<int>();
    const std::string worst = reply.at("worst").get<std::string>();
    if (worst != ipc::badge_none) {
      summary.worst = parse_severity(worst);
    }
    return summary;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("alerts.badge: ") + error.what());
  }
}

Json alerts_list_params(WallTime since) {
  return Json{{"since", unix_seconds(since)}};
}

Result<std::vector<AlertRow>> parse_alerts_list(const Json& reply) {
  try {
    std::vector<AlertRow> alerts;
    for (const Json& alert : reply.at("alerts")) {
      alerts.push_back(
          AlertRow{.rule = alert.at("rule").get<std::string>(),
                   .entity = alert.at("entity").get<std::string>(),
                   .title = alert.at("title").get<std::string>(),
                   .severity = parse_severity(alert.at("severity").get<std::string>()),
                   .first = from_unix_seconds(alert.at("first_at").get<std::int64_t>()),
                   .last = from_unix_seconds(alert.at("last_at").get<std::int64_t>()),
                   .count = alert.at("count").get<std::int64_t>(),
                   .active = alert.at("active").get<bool>(),
                   .snoozed_until = nullable_time(alert, "snoozed_until"),
                   .toast_at = nullable_time(alert, "toast_at"),
                   .detail = alert.at("detail").get<std::string>()});
    }
    return alerts;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("alerts.list: ") + error.what());
  }
}

Json alerts_snooze_params(std::string_view rule, std::string_view entity,
                          std::chrono::minutes duration) {
  return Json{{"rule", rule}, {"entity", entity}, {"minutes", duration.count()}};
}

Json alerts_dismiss_params(std::string_view rule, std::string_view entity) {
  return Json{{"rule", rule}, {"entity", entity}};
}

Result<std::vector<FreezeGroupRow>> parse_input_summary(const Json& reply) {
  try {
    std::vector<FreezeGroupRow> groups;
    for (const Json& group : reply.at("groups")) {
      FreezeGroupRow row;
      row.mechanism = group.at("mechanism").get<std::string>();
      row.process = nullable<std::string>(group, "process");
      row.count = group.at("count").get<std::int64_t>();
      row.latest = from_unix_seconds(group.at("latest").get<std::int64_t>());
      for (const Json& incident : group.at("incidents")) {
        const Json& cause = incident.at("cause");
        row.incidents.push_back(FreezeRow{
            .id = incident.at("id").get<std::int64_t>(),
            .chord = incident.at("chord").get<std::string>(),
            .time = from_unix_seconds(incident.at("time").get<std::int64_t>()),
            .latency_ms = incident.at("latency_ms").get<std::int64_t>(),
            .cause = cause.is_null() ? std::nullopt
                                     : std::optional(cause.at("image").get<std::string>()),
            .evidence = incident.at("evidence").get<std::vector<std::string>>()});
      }
      groups.push_back(std::move(row));
    }
    return groups;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("input.summary: ") + error.what());
  }
}

namespace {

FreezeRateRow rate_row(const Json& rate) {
  return FreezeRateRow{.incidents = rate.at("incidents").get<std::int64_t>(),
                       .hours = rate.at("hours").get<double>(),
                       .per_day = nullable<double>(rate, "per_day")};
}

PauseRow pause_row(const Json& pause) {
  const Json& during = pause.at("during");
  return PauseRow{
      .plan = pause.at("plan").get<std::int64_t>(),
      .image = pause.at("image").get<std::string>(),
      .mode = pause.at("mode").get<std::string>(),
      .started = from_unix_seconds(pause.at("started").get<std::int64_t>()),
      .ends = from_unix_seconds(pause.at("ends").get<std::int64_t>()),
      .restored = nullable_time(pause, "restored"),
      .end_reason = nullable<std::string>(pause, "end_reason"),
      .baseline = rate_row(pause.at("baseline")),
      .during = during.is_null() ? std::nullopt : std::optional(rate_row(during)),
      .change = nullable<std::string>(pause, "change"),
      .running = pause.contains("running") ? nullable<bool>(pause, "running") : std::nullopt};
}

}  // namespace

Result<EliminationView> parse_input_pauses(const Json& reply) {
  try {
    EliminationView view{.collecting = reply.at("collecting").get<bool>()};
    if (const Json& baseline = reply.at("baseline"); !baseline.is_null()) {
      view.baseline = rate_row(baseline);
    }
    for (const Json& suspect : reply.at("suspects")) {
      view.suspects.push_back(SuspectRow{.id = suspect.at("id").get<std::int64_t>(),
                                         .image = suspect.at("image").get<std::string>(),
                                         .hook = suspect.at("hook").get<bool>(),
                                         .mode = suspect.at("mode").get<std::string>(),
                                         .running = nullable<bool>(suspect, "running")});
    }
    if (const Json& offer = reply.at("offer"); !offer.is_null()) {
      view.offer = PauseOfferRow{.suspect = offer.at("suspect").get<std::int64_t>(),
                                 .image = offer.at("image").get<std::string>(),
                                 .mode = offer.at("mode").get<std::string>(),
                                 .hours = offer.at("hours").get<std::int64_t>()};
    }
    if (const Json& active = reply.at("active"); !active.is_null()) {
      view.active = pause_row(active);
    }
    for (const Json& pause : reply.at("pauses")) {
      view.pauses.push_back(pause_row(pause));
    }
    return view;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("input.pauses: ") + error.what());
  }
}

}  // namespace myclean::tray_model
