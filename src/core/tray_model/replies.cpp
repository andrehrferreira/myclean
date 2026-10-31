#include "core/tray_model/replies.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
                          std::int64_t to) {
  return Json{
      {"metric", metric}, {"entity", ipc::entity_token(entity)}, {"from", from}, {"to", to}};
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
        row.members.push_back(ProcessRow{
            .pid = member.at("pid").get<std::uint32_t>(),
            .image = member.at("image").get<std::string>(),
            .cpu_percent = member.at("cpu_percent").get<double>(),
            .private_working_set = Bytes{member.at("private_working_set").get<std::uint64_t>()},
            .private_bytes = Bytes{member.at("private_bytes").get<std::uint64_t>()}});
      }
      if (row.members.empty()) {
        return fail(Errc::invalid_data, "processes.top: a tree without members");
      }
      trees.push_back(std::move(row));
    }
    return trees;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("processes.top: ") + error.what());
  }
}

std::string processor_entity(unsigned index) {
  constexpr unsigned group_size = 64;
  return std::to_string(index / group_size) + "," + std::to_string(index % group_size);
}

}  // namespace myclean::tray_model
