#include "svc/telemetry_component.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/alerts/engine.hpp"
#include "core/health/analysis.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/time_series.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/gpu_collector.hpp"
#include "core/telemetry/history.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/telemetry/system_collectors.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "core/trust/trust.hpp"
#include "svc/component.hpp"
#include "svc/trust_component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;
using std::chrono::seconds;

constexpr std::int64_t default_top_trees = 10;
constexpr std::int64_t default_max_points = 1000;
constexpr std::int64_t max_points_limit = 10'000;
constexpr std::int64_t latest_time = 253'402'300'799;  // 9999-12-31T23:59:59Z

std::int64_t unix_seconds(WallTime time) {
  return std::chrono::duration_cast<seconds>(time.time_since_epoch()).count();
}

double percent(Bytes part, Bytes whole) {
  return whole.count() == 0
             ? 0.0
             : 100.0 * static_cast<double>(part.count()) / static_cast<double>(whole.count());
}

// Wraps `collector` in a Tap, adds it to the pipeline and returns the inner collector.
template <typename T>
T* add_tapped(telemetry::Pipeline& pipeline, Collectors& collectors, std::unique_ptr<T> collector) {
  T* inner = collector.get();
  auto tap = std::make_unique<Tap>(std::move(collector));
  collectors.taps.push_back(tap.get());
  pipeline.add(std::move(tap));
  return inner;
}

// The same, or records why the collector is unavailable.
template <typename T>
T* add_tapped(telemetry::Pipeline& pipeline, Collectors& collectors, std::string_view name,
              Result<std::unique_ptr<T>> opened) {
  if (!opened) {
    collectors.unavailable.push_back(Error{
        .code = opened.error().code, .message = std::string(name) + ": " + opened.error().message});
    return nullptr;
  }
  return add_tapped(pipeline, collectors, std::move(*opened));
}

// Natural order of logical processors "<group>,<number>" ("0,2" before "0,10" before "1,0").
bool processor_before(const std::string& left, const std::string& right) {
  const auto split = [](const std::string& entity) {
    const std::size_t comma = entity.find(',');
    const std::string group = entity.substr(0, comma);
    const std::string number = comma == std::string::npos ? "" : entity.substr(comma + 1);
    return std::tuple{group.size(), group, number.size(), number};
  };
  return split(left) < split(right);
}

// Natural order of disk numbers ("2" before "10").
bool disk_before(const std::string& left, const std::string& right) {
  return left.size() != right.size() ? left.size() < right.size() : left < right;
}

std::string_view fill_state_name(telemetry::FillState state) {
  switch (state) {
    case telemetry::FillState::filling:
      return "filling";
    case telemetry::FillState::not_filling:
      return "not_filling";
    case telemetry::FillState::insufficient_data:
      return "insufficient_data";
  }
  return "insufficient_data";
}

// The members of processes.top's trees.
Json members_json(const telemetry::ProcessView& view, const telemetry::ProcessTree& tree) {
  Json members = Json::array();
  for (const std::size_t index : tree.members) {
    const telemetry::ProcessRow& row = view.rows[index];
    members.push_back(Json{{"pid", row.info.pid},
                           {"parent_pid", row.info.parent_pid},
                           {"image", row.info.image},
                           {"cpu_percent", row.cpu_percent},
                           {"private_working_set", row.info.private_working_set.count()},
                           {"private_bytes", row.info.private_bytes.count()}});
  }
  return members;
}

// Merges consecutive points so that at most `limit` remain: min of the minimums, max of the
// maximums, the count-weighted mean, the time of the first.
std::vector<store::Aggregate> merge_points(const std::vector<store::Aggregate>& points,
                                           std::size_t limit) {
  if (points.size() <= limit) {
    return points;
  }
  const std::size_t per_bucket = (points.size() + limit - 1) / limit;
  std::vector<store::Aggregate> merged;
  for (std::size_t first = 0; first < points.size(); first += per_bucket) {
    const std::size_t last = std::min(points.size(), first + per_bucket);
    store::Aggregate bucket = points[first];
    double sum = points[first].mean * static_cast<double>(points[first].count);
    for (std::size_t index = first + 1; index < last; ++index) {
      const store::Aggregate& point = points[index];
      bucket.min = std::min(bucket.min, point.min);
      bucket.max = std::max(bucket.max, point.max);
      bucket.count += point.count;
      sum += point.mean * static_cast<double>(point.count);
    }
    bucket.mean = bucket.count > 0 ? sum / static_cast<double>(bucket.count) : bucket.mean;
    merged.push_back(bucket);
  }
  return merged;
}

}  // namespace

Result<std::vector<telemetry::Measurement>> Tap::collect(WallTime now, MonoTime monotonic) {
  Result<std::vector<telemetry::Measurement>> measurements = inner_->collect(now, monotonic);
  if (measurements) {
    latest_.publish(Reading{.time = now, .measurements = *measurements});
  }
  return measurements;
}

Collectors add_collectors(telemetry::Pipeline& pipeline, TelemetrySources& sources) {
  Collectors collectors;
  collectors.live.processes = add_tapped(
      pipeline, collectors, std::make_unique<telemetry::ProcessCollector>(*sources.processes));
  add_tapped(pipeline, collectors, "cpu", telemetry::make_cpu_collector(*sources.counters));
  add_tapped(pipeline, collectors, "memory",
             telemetry::make_memory_collector(*sources.memory, *sources.counters));
  add_tapped(pipeline, collectors, "disks", telemetry::make_disk_collector(*sources.counters));
  collectors.live.gpu =
      add_tapped(pipeline, collectors, "gpu", telemetry::GpuCollector::open(*sources.counters));
  collectors.live.volumes = add_tapped(
      pipeline, collectors, std::make_unique<telemetry::VolumeCollector>(*sources.volumes));
  return collectors;
}

TelemetryMethods::TelemetryMethods(const Collectors& collectors, store::Store& store,
                                   const os::IClock& clock, const TelemetrySources& sources,
                                   ProcessTrust trust)
    : collectors_(&collectors),
      store_(&store),
      clock_(&clock),
      memory_(sources.memory.get()),
      logical_processors_(sources.processes->logical_processors()),
      system_drive_(sources.system_drive),
      trust_(std::move(trust)) {}

Result<void> TelemetryMethods::register_methods(ipc::Dispatcher& methods) const {
  using ipc::Param;
  using ipc::ParamType;
  return methods
      .add(std::string(ipc::metrics_latest_method), {}, [this](const Json&) { return latest(); })
      .and_then([&] {
        return methods.add(std::string(ipc::metrics_query_method),
                           {Param{.name = "metric", .type = ParamType::token, .required = true},
                            Param{.name = "entity", .type = ParamType::token},
                            Param{.name = "from", .type = ParamType::integer, .required = true},
                            Param{.name = "to", .type = ParamType::integer},
                            Param{.name = "max_points", .type = ParamType::integer}},
                           [this](const Json& params) { return query(params); });
      })
      .and_then([&] {
        return methods.add(std::string(ipc::processes_top_method),
                           {Param{.name = "sort", .type = ParamType::token},
                            Param{.name = "limit", .type = ParamType::integer},
                            Param{.name = "qualifying", .type = ParamType::boolean}},
                           [this](const Json& params) { return top(params); });
      })
      .and_then([&] {
        return methods.add(std::string(ipc::processes_explain_method),
                           {Param{.name = "pid", .type = ParamType::integer, .required = true}},
                           [this](const Json& params) { return explain(params); });
      })
      .and_then([&] {
        return methods.add(std::string(ipc::volumes_list_method), {},
                           [this](const Json&) { return volumes(); });
      });
}

Result<Json> TelemetryMethods::latest() const {
  Json result{{"cpu", Json::object()}, {"memory", Json::object()}, {"process", Json::object()}};
  std::map<std::string, Json, decltype(&disk_before)> disks(&disk_before);
  std::map<std::string, Json, decltype(&processor_before)> processors(&processor_before);
  std::map<std::string, Json> adapters;
  WallTime newest{};
  for (const Tap* tap : collectors_->taps) {
    const std::shared_ptr<const Reading> reading = tap->latest();
    if (!reading) {
      continue;
    }
    newest = std::max(newest, reading->time);
    for (const telemetry::Measurement& measurement : reading->measurements) {
      const std::string& metric = measurement.key.metric;
      const std::string& entity = measurement.key.entity;
      const std::size_t dot = metric.find('.');
      const std::string group = metric.substr(0, dot);
      const std::string field = metric.substr(dot + 1);
      if (entity == telemetry::metric::system) {
        result[group][field] = measurement.value;
      } else if (group == "disk") {
        disks[entity][field] = measurement.value;
      } else if (group == "cpu") {
        processors[entity][field] = measurement.value;
      } else if (metric == telemetry::metric::gpu_engine) {
        const std::size_t slash = entity.rfind('/');
        adapters[entity.substr(0, slash)]["engines"][entity.substr(slash + 1)] = measurement.value;
      } else if (group == "gpu") {
        adapters[entity][field] = measurement.value;
      }
    }
  }
  result["cpu"]["logical_processors"] = logical_processors_;
  Json processor_list = Json::array();
  for (auto& [processor, values] : processors) {
    values["entity"] = ipc::entity_token(processor);
    processor_list.push_back(std::move(values));
  }
  result["cpu"]["processors"] = std::move(processor_list);
  result["time"] = unix_seconds(newest);

  Json disk_list = Json::array();
  for (auto& [disk, values] : disks) {
    values["disk"] = disk;
    disk_list.push_back(std::move(values));
  }
  result["disks"] = std::move(disk_list);

  if (collectors_->live.gpu == nullptr) {
    result["gpu"] = nullptr;
  } else {
    Json adapter_list = Json::array();
    for (auto& [adapter, values] : adapters) {
      values["adapter"] = adapter;
      values["entity"] = ipc::entity_token(adapter);
      adapter_list.push_back(std::move(values));
    }
    result["gpu"] = Json{{"adapters", std::move(adapter_list)}};
  }

  Json unavailable = Json::array();
  for (const Error& error : collectors_->unavailable) {
    unavailable.push_back(error.message);
  }
  result["unavailable"] = std::move(unavailable);

  Result<Json> volumes = volume_list();
  if (!volumes) {
    return volumes;
  }
  result["volumes"] = std::move(*volumes);
  return result;
}

Result<Json> TelemetryMethods::query(const Json& params) const {
  const std::string metric = params.at("metric").get<std::string>();
  const std::string token = params.value("entity", std::string(telemetry::metric::system));
  const WallTime now = clock_->now();
  const std::int64_t from = params.at("from").get<std::int64_t>();
  const std::int64_t to = params.value("to", unix_seconds(now) + 1);  // the newest sample too
  const std::int64_t max_points = params.value("max_points", default_max_points);
  if (from < 0 || to > latest_time || from >= to) {
    return fail(Errc::invalid_argument,
                "metrics.query: from and to must be Unix seconds, with from before to");
  }
  if (max_points < 1 || max_points > max_points_limit) {
    return fail(Errc::invalid_argument,
                "metrics.query: max_points must be 1 to " + std::to_string(max_points_limit));
  }

  std::set<std::string> metrics;
  std::optional<std::string> entity;
  for (const Tap* tap : collectors_->taps) {
    const std::shared_ptr<const Reading> reading = tap->latest();
    for (const telemetry::Measurement& measurement :
         reading ? reading->measurements : std::vector<telemetry::Measurement>{}) {
      metrics.insert(measurement.key.metric);
      if (!entity && measurement.key.metric == metric &&
          ipc::entity_token(measurement.key.entity) == token) {
        entity = measurement.key.entity;
      }
    }
  }
  if (!metrics.contains(metric)) {
    std::string known;
    for (const std::string& name : metrics) {
      known += (known.empty() ? "" : ", ") + name;
    }
    return fail(Errc::invalid_argument,
                "metrics.query: unknown metric '" + metric + "'; known metrics: " + known);
  }
  // An entity not measured now (a disk unplugged since) is looked up as given.
  const SeriesKey key{.metric = metric, .entity = entity.value_or(token)};
  const Result<telemetry::History> history =
      telemetry::query_history(*store_, key, WallTime{seconds{from}}, WallTime{seconds{to}}, now);
  if (!history) {
    return tl::unexpected(history.error());
  }
  Json points = Json::array();
  for (const store::Aggregate& point :
       merge_points(history->points, static_cast<std::size_t>(max_points))) {
    points.push_back(Json{{"time", unix_seconds(point.time)},
                          {"min", point.min},
                          {"max", point.max},
                          {"mean", point.mean},
                          {"count", point.count}});
  }
  return Json{{"metric", key.metric},
              {"entity", key.entity},
              {"resolution", telemetry::to_string(history->resolution)},
              {"points", std::move(points)}};
}

Result<Json> TelemetryMethods::top(const Json& params) const {
  const std::string sort = params.value("sort", std::string("cpu"));
  const std::int64_t limit = params.value("limit", std::int64_t{default_top_trees});
  const bool qualifying = params.value("qualifying", false);
  if (sort != "cpu" && sort != "memory") {
    return fail(Errc::invalid_argument, R"(processes.top: sort must be "cpu" or "memory")");
  }
  if (limit < 1 || limit > ipc::max_top_trees) {
    return fail(Errc::invalid_argument,
                "processes.top: limit must be 1 to " + std::to_string(ipc::max_top_trees));
  }
  const std::shared_ptr<const telemetry::ProcessView> view =
      collectors_->live.processes != nullptr ? collectors_->live.processes->latest() : nullptr;
  if (!view) {
    return fail(Errc::invalid_state, "processes.top: the process table has not been sampled yet");
  }
  const Result<telemetry::MemoryStatus> memory = memory_->read();
  if (!memory) {
    return tl::unexpected(memory.error());
  }

  // PID 0, the System Idle Process, is idle time, not a consumer.
  std::size_t total = 0;
  std::vector<const telemetry::ProcessTree*> trees;
  for (const telemetry::ProcessTree& tree : view->trees) {
    if (view->rows[tree.members.front()].info.pid == 0) {
      continue;
    }
    ++total;
    if (!qualifying || tree.cpu_percent > ipc::qualifying_cpu_percent ||
        percent(tree.private_working_set, memory->total) > ipc::qualifying_memory_percent) {
      trees.push_back(&tree);
    }
  }
  if (sort == "memory") {
    std::ranges::stable_sort(trees, [](const auto* left, const auto* right) {
      return left->private_working_set > right->private_working_set;
    });
  } else {
    std::ranges::stable_sort(trees, [](const auto* left, const auto* right) {
      return left->cpu_percent > right->cpu_percent;
    });
  }
  trees.resize(std::min(trees.size(), static_cast<std::size_t>(limit)));

  Json list = Json::array();
  for (const telemetry::ProcessTree* tree : trees) {
    const telemetry::ProcessInfo& root = view->rows[tree->members.front()].info;
    list.push_back(Json{{"pid", root.pid},
                        {"image", root.image},
                        {"processes", tree->members.size()},
                        {"cpu_percent", tree->cpu_percent},
                        {"memory_percent", percent(tree->private_working_set, memory->total)},
                        {"private_working_set", tree->private_working_set.count()},
                        {"private_bytes", tree->private_bytes.count()},
                        {"working_set", tree->working_set.count()},
                        {"io_bytes_per_second", tree->io_bytes_per_second},
                        {"handles", tree->handles},
                        {"threads", tree->threads},
                        {"members", members_json(*view, *tree)},
                        {"trust", trust_of(root)}});
  }
  return Json{{"time", unix_seconds(view->time)},
              {"logical_processors", logical_processors_},
              {"memory_total", memory->total.count()},
              {"filter", qualifying ? Json{{"cpu_percent", ipc::qualifying_cpu_percent},
                                           {"memory_percent", ipc::qualifying_memory_percent}}
                                    : Json(nullptr)},
              {"total_trees", total},
              {"trees", std::move(list)}};
}

Result<Json> TelemetryMethods::explain(const Json& params) const {
  const auto pid = params.at("pid").get<std::int64_t>();
  if (pid < 0 || pid > std::numeric_limits<std::uint32_t>::max()) {
    return fail(Errc::invalid_argument, "processes.explain: pid must be 0 to 4294967295");
  }
  const std::shared_ptr<const telemetry::ProcessView> view =
      collectors_->live.processes != nullptr ? collectors_->live.processes->latest() : nullptr;
  if (!view) {
    return fail(Errc::invalid_state,
                "processes.explain: the process table has not been sampled yet");
  }
  const auto row =
      std::ranges::find(view->rows, static_cast<std::uint32_t>(pid),
                        [](const telemetry::ProcessRow& entry) { return entry.info.pid; });
  if (row == view->rows.end()) {
    return fail(Errc::not_found, "process " + std::to_string(pid) + " was not found");
  }
  const auto index = static_cast<std::size_t>(row - view->rows.begin());
  const auto tree = std::ranges::find_if(view->trees, [index](const telemetry::ProcessTree& entry) {
    return std::ranges::find(entry.members, index) != entry.members.end();
  });
  const Result<telemetry::MemoryStatus> memory = memory_->read();
  if (!memory) {
    return tl::unexpected(memory.error());
  }
  const telemetry::ProcessInfo& info = row->info;
  const std::optional<trust::TrustSignal> signal = signal_of(info);
  Json trust = signal ? trust_json(*signal) : Json(nullptr);
  Json kb = nullptr;
  if (signal && signal->entry) {
    kb = Json{{"description", signal->entry->description},
              {"category", signal->entry->category},
              {"needed_at_startup", signal->entry->needed_at_startup},
              {"safe_to_remove", signal->entry->safe_to_remove}};
  }
  Json result{{"pid", info.pid},
              {"parent_pid", info.parent_pid},
              {"image", info.image},
              {"path", signal ? trust.at("path") : Json(nullptr)},
              {"cpu_percent", row->cpu_percent},
              {"memory_percent", percent(info.private_working_set, memory->total)},
              {"private_working_set", info.private_working_set.count()},
              {"private_bytes", info.private_bytes.count()},
              {"working_set", info.working_set.count()},
              {"tree", nullptr},
              {"members", Json::array()},
              {"trust", std::move(trust)},
              {"kb", std::move(kb)}};
  if (tree != view->trees.end()) {
    const telemetry::ProcessInfo& root = view->rows[tree->members.front()].info;
    result["tree"] = Json{{"pid", root.pid},
                          {"image", root.image},
                          {"processes", tree->members.size()},
                          {"cpu_percent", tree->cpu_percent},
                          {"memory_percent", percent(tree->private_working_set, memory->total)}};
    result["members"] = members_json(*view, *tree);
  }
  return result;
}

std::optional<trust::TrustSignal> TelemetryMethods::signal_of(
    const telemetry::ProcessInfo& process) const {
  if (!trust_) {
    return std::nullopt;
  }
  Result<trust::TrustSignal> signal = trust_(process.pid, process.created);
  return signal ? std::optional<trust::TrustSignal>{*std::move(signal)} : std::nullopt;
}

Json TelemetryMethods::trust_of(const telemetry::ProcessInfo& process) const {
  const std::optional<trust::TrustSignal> signal = signal_of(process);
  return signal ? trust_json(*signal) : Json(nullptr);
}

Result<std::vector<alerts::Reading>> TelemetryMethods::volume_facts() const {
  const std::shared_ptr<const telemetry::VolumeView> view =
      collectors_->live.volumes != nullptr ? collectors_->live.volumes->latest() : nullptr;
  std::vector<alerts::Reading> facts;
  if (!view) {
    return facts;
  }
  constexpr double gib = 1024.0 * 1024.0 * 1024.0;
  const WallTime now = clock_->now();
  for (const telemetry::VolumeStatus& volume : view->volumes) {
    const std::string entity = telemetry::volume_entity(volume);
    const std::string free =
        std::format("{:.1f} GiB free of {:.1f} GiB", static_cast<double>(volume.free.count()) / gib,
                    static_cast<double>(volume.size.count()) / gib);
    facts.push_back(alerts::Reading{.metric = "volume.free_percent",
                                    .entity = entity,
                                    .value = percent(volume.free, volume.size),
                                    .detail = free});
    const Result<telemetry::FillProjection> fill =
        telemetry::project_volume_fill(*store_, entity, now);
    if (!fill) {
      return tl::unexpected(fill.error());
    }
    if (fill->state == telemetry::FillState::filling && fill->days_until_full) {
      facts.push_back(
          alerts::Reading{.metric = "volume.days_until_full",
                          .entity = entity,
                          .value = *fill->days_until_full,
                          .detail = std::format("{}; full in {:.0f} days at the current rate", free,
                                                *fill->days_until_full)});
    } else if (fill->state == telemetry::FillState::not_filling) {
      facts.push_back(alerts::Reading{.metric = "volume.days_until_full",
                                      .entity = entity,
                                      .value = std::numeric_limits<double>::infinity(),
                                      .detail = free + "; not filling"});
    }
  }
  return facts;
}

Result<Json> TelemetryMethods::volumes() const {
  const std::shared_ptr<const telemetry::VolumeView> view = collectors_->live.volumes->latest();
  return volume_list().transform([&](Json list) {
    return Json{{"time", view ? unix_seconds(view->time) : 0}, {"volumes", std::move(list)}};
  });
}

Result<Json> TelemetryMethods::volume_list() const {
  const std::shared_ptr<const telemetry::VolumeView> view = collectors_->live.volumes->latest();
  Json list = Json::array();
  if (!view) {
    return list;
  }
  const WallTime now = clock_->now();
  for (const telemetry::VolumeStatus& volume : view->volumes) {
    const std::string entity = telemetry::volume_entity(volume);
    const Result<telemetry::FillProjection> fill =
        telemetry::project_volume_fill(*store_, entity, now);
    if (!fill) {
      return tl::unexpected(fill.error());
    }
    list.push_back(
        Json{{"volume", entity},
             {"entity", ipc::entity_token(entity)},
             {"mount_points", volume.mount_points},
             {"size", volume.size.count()},
             {"free", volume.free.count()},
             {"free_percent", percent(volume.free, volume.size)},
             {"disks", volume.disks},
             {"system", entity == system_drive_},
             {"fill", Json{{"state", fill_state_name(fill->state)},
                           {"bytes_per_day", fill->bytes_per_day},
                           {"days_until_full", fill->days_until_full ? Json(*fill->days_until_full)
                                                                     : Json(nullptr)}}}});
  }
  return list;
}

TelemetryComponent::TelemetryComponent(TelemetrySources sources, ProcessTrust trust,
                                       health::IAlertSink* alerts)
    : sources_(std::move(sources)), trust_(std::move(trust)), alerts_(alerts) {}

TelemetryComponent::~TelemetryComponent() {
  stop();
}

Result<void> TelemetryComponent::start(const Context& context) {
  clock_ = &context.clock();
  pipeline_ = std::make_unique<telemetry::Pipeline>(context.clock(), context.store());
  collectors_ = add_collectors(*pipeline_, sources_);
  for (const Error& error : collectors_.unavailable) {
    spdlog::warn("telemetry: collector unavailable: {}", error.message);
  }
  methods_ = std::make_unique<TelemetryMethods>(collectors_, context.store(), context.clock(),
                                                sources_, trust_);
  if (Result<void> registered = methods_->register_methods(context.methods()); !registered) {
    return registered;
  }
  poll();
  thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  return {};
}

void TelemetryComponent::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void TelemetryComponent::poll() {
  const std::vector<Error> errors = pipeline_->poll();
  report_volumes();
  const MonoTime now = clock_->monotonic();
  const std::scoped_lock lock(mutex_);
  last_poll_ = now;
  for (const Error& error : errors) {
    const std::string source = error.message.substr(0, error.message.find(": "));
    Failure& failure = failures_[source];
    if (failure.message != error.message) {
      spdlog::warn("telemetry: {}", error.message);
    }
    failure = Failure{.message = error.message, .time = now};
  }
  std::erase_if(failures_, [&](const auto& entry) {
    const bool recovered = now - entry.second.time > failure_window;
    if (recovered) {
      spdlog::info("telemetry: {} recovered", entry.first);
    }
    return recovered;
  });
}

void TelemetryComponent::run(const std::stop_token& stop) {
  // Never sleep longer than this, so a test clock that does not move still polls.
  constexpr MonoTime::duration max_wait = seconds{1};
  std::mutex wait_mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(wait_mutex);
  while (!stop.stop_requested()) {
    const std::optional<MonoTime> due = pipeline_->next_due();
    const MonoTime::duration wait =
        due ? std::clamp(*due - clock_->monotonic(), MonoTime::duration::zero(), max_wait)
            : max_wait;
    static_cast<void>(wake.wait_for(lock, stop, wait, [] { return false; }));
    if (stop.stop_requested()) {
      break;
    }
    poll();
  }
}

void TelemetryComponent::report_volumes() {
  const std::shared_ptr<const telemetry::VolumeView> view = volumes();
  if (alerts_ == nullptr || !view || volumes_reported_ == view->time) {
    return;
  }
  volumes_reported_ = view->time;
  Result<std::vector<alerts::Reading>> facts = methods_->volume_facts();
  if (!facts) {
    spdlog::warn("telemetry: volume alert facts: {}", facts.error().message);
    return;
  }
  alerts_->report(*facts);
}

std::shared_ptr<const telemetry::VolumeView> TelemetryComponent::volumes() const {
  return collectors_.live.volumes != nullptr ? collectors_.live.volumes->latest() : nullptr;
}

ipc::ComponentHealth TelemetryComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  for (const Error& error : collectors_.unavailable) {
    health.unavailable.push_back(error.message);
  }
  const std::scoped_lock lock(mutex_);
  const MonoTime now = clock_->monotonic();
  for (const auto& [source, failure] : failures_) {
    if (now - failure.time <= failure_window) {
      health.problems.push_back(failure.message);
    }
  }
  if (last_poll_ && now - *last_poll_ > stall_limit) {
    health.problems.push_back(
        "no sample for " +
        std::to_string(std::chrono::duration_cast<seconds>(now - *last_poll_).count()) + " s");
  }
  health.healthy = health.problems.empty();
  return health;
}

}  // namespace myclean::svc
