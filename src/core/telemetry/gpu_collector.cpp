#include "core/telemetry/gpu_collector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::telemetry {

namespace {

enum Counter : std::size_t {
  engine_utilization,
  adapter_dedicated,
  adapter_shared,
  process_dedicated,
  process_shared
};

constexpr std::array<std::string_view, GpuCollector::counter_count> paths{
    R"(\GPU Engine(*)\Utilization Percentage)", R"(\GPU Adapter Memory(*)\Dedicated Usage)",
    R"(\GPU Adapter Memory(*)\Shared Usage)", R"(\GPU Process Memory(*)\Dedicated Usage)",
    R"(\GPU Process Memory(*)\Shared Usage)"};

constexpr std::string_view engine_marker = "_eng_";
constexpr std::string_view type_marker = "_engtype_";

struct PidInstance {
  std::uint32_t pid = 0;
  std::string_view rest;  // after "pid_<n>_"
};

// "pid_1234_luid_0x00000000_0x0000D1B5_phys_0..." -> 1234 and "luid_...".
std::optional<PidInstance> split_pid(std::string_view instance) {
  constexpr std::string_view prefix = "pid_";
  if (!instance.starts_with(prefix)) {
    return std::nullopt;
  }
  instance.remove_prefix(prefix.size());
  constexpr std::size_t max_digits = 10;
  const std::size_t end = instance.find('_');
  if (end == 0 || end > max_digits || end == std::string_view::npos) {
    return std::nullopt;
  }
  std::uint64_t pid = 0;
  for (const char digit : instance.substr(0, end)) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    pid = (pid * 10) + static_cast<std::uint64_t>(digit - '0');
  }
  if (pid > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return PidInstance{.pid = static_cast<std::uint32_t>(pid), .rest = instance.substr(end + 1)};
}

struct EngineInstance {
  std::uint32_t pid = 0;
  std::string_view adapter;  // "luid_0x00000000_0x0000D1B5_phys_0"
  std::string_view engine;   // adapter + "_eng_<n>"
  std::string_view type;     // "3D", "Video Decode", ...
};

std::optional<EngineInstance> parse_engine(std::string_view instance) {
  const std::optional<PidInstance> pid = split_pid(instance);
  if (!pid) {
    return std::nullopt;
  }
  const std::size_t engine = pid->rest.find(engine_marker);
  const std::size_t type = pid->rest.find(type_marker);
  if (engine == std::string_view::npos || type == std::string_view::npos || type < engine) {
    return std::nullopt;
  }
  return EngineInstance{.pid = pid->pid,
                        .adapter = pid->rest.substr(0, engine),
                        .engine = pid->rest.substr(0, type),
                        .type = pid->rest.substr(type + type_marker.size())};
}

Bytes to_bytes(double value) {
  return Bytes{value > 0.0 ? static_cast<std::uint64_t>(value) : 0U};
}

// A process's use while it is being summed up.
struct ProcessUse {
  std::map<std::string, double, std::less<>> engines;  // by type
  Bytes dedicated;
  Bytes shared;
};

struct EngineLoad {
  std::string_view adapter;
  std::string_view type;
  double percent = 0.0;
};

}  // namespace

Result<std::unique_ptr<GpuCollector>> GpuCollector::open(IPerfCounters& counters) {
  Result<std::unique_ptr<ICounterQuery>> query = counters.open();
  if (!query) {
    return tl::unexpected(query.error());
  }
  Indices indices{};
  for (std::size_t counter = 0; counter < paths.size(); ++counter) {
    const Result<std::size_t> index = (*query)->add(paths.at(counter));
    if (!index) {
      return fail(index.error().code,
                  std::string(paths.at(counter)) + ": " + index.error().message);
    }
    indices.at(counter) = *index;
  }
  return std::make_unique<GpuCollector>(std::move(*query), indices);
}

GpuCollector::GpuCollector(std::unique_ptr<ICounterQuery> query, Indices indices)
    : query_(std::move(query)), indices_(indices) {}

Result<std::vector<Measurement>> GpuCollector::collect(WallTime now, MonoTime /*monotonic*/) {
  if (Result<void> collected = query_->collect(); !collected) {
    return tl::unexpected(collected.error());
  }
  std::array<std::vector<CounterValue>, counter_count> values;
  for (std::size_t counter = 0; counter < values.size(); ++counter) {
    Result<std::vector<CounterValue>> read = query_->values(indices_.at(counter));
    if (!read) {
      return tl::unexpected(read.error());
    }
    values.at(counter) = std::move(*read);
  }

  std::map<std::uint32_t, ProcessUse> processes;
  std::map<std::string_view, EngineLoad> engines;  // by engine
  for (const CounterValue& value : values[engine_utilization]) {
    const std::optional<EngineInstance> instance = parse_engine(value.instance);
    if (!instance) {
      continue;
    }
    EngineLoad& engine = engines[instance->engine];
    engine.adapter = instance->adapter;
    engine.type = instance->type;
    engine.percent += value.value;
    ProcessUse& process = processes[instance->pid];
    double& type = process.engines[std::string(instance->type)];
    type = std::max(type, std::min(value.value, 100.0));
  }
  for (const auto counter : {process_dedicated, process_shared}) {
    for (const CounterValue& value : values.at(counter)) {
      const std::optional<PidInstance> instance = split_pid(value.instance);
      if (!instance) {
        continue;
      }
      ProcessUse& process = processes[instance->pid];
      (counter == process_dedicated ? process.dedicated : process.shared) += to_bytes(value.value);
    }
  }

  std::map<std::string, double> by_type;     // "adapter/type" -> %
  std::map<std::string, double> by_adapter;  // adapter -> %
  for (const auto& [name, engine] : engines) {
    const double percent = std::clamp(engine.percent, 0.0, 100.0);
    double& type = by_type[std::string(engine.adapter) + "/" + std::string(engine.type)];
    type = std::max(type, percent);
    double& adapter = by_adapter[std::string(engine.adapter)];
    adapter = std::max(adapter, percent);
  }
  std::vector<Measurement> measurements;
  measurements.reserve(by_type.size() + by_adapter.size() + values[adapter_dedicated].size() +
                       values[adapter_shared].size());
  for (auto& [entity, percent] : by_type) {
    measurements.push_back(measure(metric::gpu_engine, entity, percent));
  }
  for (auto& [entity, percent] : by_adapter) {
    measurements.push_back(measure(metric::gpu_utilization, entity, percent));
  }
  for (const CounterValue& value : values[adapter_dedicated]) {
    measurements.push_back(measure(metric::gpu_dedicated, value.instance, value.value));
  }
  for (const CounterValue& value : values[adapter_shared]) {
    measurements.push_back(measure(metric::gpu_shared, value.instance, value.value));
  }

  GpuView view{.time = now};
  view.processes.reserve(processes.size());
  for (const auto& [pid, use] : processes) {
    GpuProcess process{.pid = pid, .dedicated = use.dedicated, .shared = use.shared};
    process.engines.reserve(use.engines.size());
    for (const auto& [type, percent] : use.engines) {
      process.engines.push_back(GpuEngineUse{.type = type, .percent = percent});
    }
    view.processes.push_back(std::move(process));
  }
  latest_.publish(std::move(view));
  return measurements;
}

}  // namespace myclean::telemetry
