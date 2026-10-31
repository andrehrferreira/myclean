#include "core/telemetry/system_collectors.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::telemetry {

namespace {

struct CounterSpec {
  std::string_view path;
  std::string_view metric;
  double scale = 1.0;
  bool percent = false;  // clamped to 0-100
};

// The entity of a counter instance, or nullopt to skip the instance.
using EntityOf = std::optional<std::string> (*)(std::string_view instance);

constexpr std::array cpu_counters{
    CounterSpec{.path = R"(\Processor Information(*)\% Processor Utility)",
                .metric = metric::cpu_utility,
                .percent = true},
    CounterSpec{.path = R"(\Processor Information(*)\% of Maximum Frequency)",
                .metric = metric::cpu_frequency},
    CounterSpec{.path = R"(\Processor Information(_Total)\% DPC Time)",
                .metric = metric::cpu_dpc,
                .percent = true},
    CounterSpec{.path = R"(\Processor Information(_Total)\% Interrupt Time)",
                .metric = metric::cpu_interrupt,
                .percent = true},
};

constexpr std::array disk_counters{
    CounterSpec{.path = R"(\PhysicalDisk(*)\Disk Read Bytes/sec)",
                .metric = metric::disk_read_bytes},
    CounterSpec{.path = R"(\PhysicalDisk(*)\Disk Write Bytes/sec)",
                .metric = metric::disk_write_bytes},
    CounterSpec{.path = R"(\PhysicalDisk(*)\Disk Transfers/sec)", .metric = metric::disk_iops},
    CounterSpec{.path = R"(\PhysicalDisk(*)\Avg. Disk sec/Read)",
                .metric = metric::disk_read_latency,
                .scale = 1000.0},
    CounterSpec{.path = R"(\PhysicalDisk(*)\Avg. Disk sec/Write)",
                .metric = metric::disk_write_latency,
                .scale = 1000.0},
    CounterSpec{.path = R"(\PhysicalDisk(*)\Current Disk Queue Length)",
                .metric = metric::disk_queue},
    CounterSpec{
        .path = R"(\PhysicalDisk(*)\% Idle Time)", .metric = metric::disk_idle, .percent = true},
};

// Standby is the sum of the first three, its priority bands.
constexpr std::array<std::string_view, 5> memory_counters{
    R"(\Memory\Standby Cache Core Bytes)", R"(\Memory\Standby Cache Normal Priority Bytes)",
    R"(\Memory\Standby Cache Reserve Bytes)", R"(\Memory\Modified Page List Bytes)",
    R"(\Memory\Pages Input/sec)"};

// "_Total" and counters without instances are the system; "<group>,_Total" is skipped.
std::optional<std::string> cpu_entity(std::string_view instance) {
  if (instance.empty() || instance == "_Total") {
    return std::string(metric::system);
  }
  if (instance.ends_with(",_Total")) {
    return std::nullopt;
  }
  return std::string(instance);
}

// "2 D: E:" is disk 2; "_Total" is skipped.
std::optional<std::string> disk_entity(std::string_view instance) {
  const auto digits = std::ranges::find_if_not(
      instance, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
  if (digits == instance.begin()) {
    return std::nullopt;
  }
  return std::string(instance.begin(), digits);
}

Result<std::size_t> add_counter(ICounterQuery& query, std::string_view path) {
  Result<std::size_t> index = query.add(path);
  if (!index) {
    return fail(index.error().code, std::string(path) + ": " + index.error().message);
  }
  return index;
}

// The single value of a counter without instances, when it has one.
Result<std::optional<double>> scalar(const ICounterQuery& query, std::size_t index) {
  Result<std::vector<CounterValue>> values = query.values(index);
  if (!values) {
    return tl::unexpected(values.error());
  }
  if (values->empty()) {
    return std::optional<double>{};
  }
  return std::optional<double>{values->front().value};
}

class CounterCollector final : public ICollector {
 public:
  CounterCollector(std::string_view name, std::unique_ptr<ICounterQuery> query,
                   std::span<const CounterSpec> specs, std::vector<std::size_t> indices,
                   EntityOf entity_of)
      : name_(name),
        query_(std::move(query)),
        specs_(specs),
        indices_(std::move(indices)),
        entity_of_(entity_of) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  [[nodiscard]] std::chrono::seconds interval() const override { return std::chrono::seconds{1}; }

  [[nodiscard]] Result<std::vector<Measurement>> collect(WallTime /*now*/,
                                                         MonoTime /*monotonic*/) override {
    if (Result<void> collected = query_->collect(); !collected) {
      return tl::unexpected(collected.error());
    }
    std::vector<Measurement> measurements;
    for (std::size_t counter = 0; counter < specs_.size(); ++counter) {
      const CounterSpec& spec = specs_[counter];
      Result<std::vector<CounterValue>> values = query_->values(indices_[counter]);
      if (!values) {
        return tl::unexpected(values.error());
      }
      for (const CounterValue& value : *values) {
        std::optional<std::string> entity = entity_of_(value.instance);
        if (!entity) {
          continue;
        }
        double scaled = value.value * spec.scale;
        if (spec.percent) {
          scaled = std::clamp(scaled, 0.0, 100.0);
        }
        measurements.push_back(measure(spec.metric, std::move(*entity), scaled));
      }
    }
    return measurements;
  }

 private:
  std::string_view name_;
  std::unique_ptr<ICounterQuery> query_;
  std::span<const CounterSpec> specs_;
  std::vector<std::size_t> indices_;
  EntityOf entity_of_;
};

Result<std::unique_ptr<ICollector>> make_counter_collector(IPerfCounters& counters,
                                                           std::string_view name,
                                                           std::span<const CounterSpec> specs,
                                                           EntityOf entity_of) {
  Result<std::unique_ptr<ICounterQuery>> query = counters.open();
  if (!query) {
    return tl::unexpected(query.error());
  }
  std::vector<std::size_t> indices;
  for (const CounterSpec& spec : specs) {
    Result<std::size_t> index = add_counter(**query, spec.path);
    if (!index) {
      return tl::unexpected(index.error());
    }
    indices.push_back(*index);
  }
  return std::make_unique<CounterCollector>(name, std::move(*query), specs, std::move(indices),
                                            entity_of);
}

class MemoryCollector final : public ICollector {
 public:
  using Indices = std::array<std::size_t, memory_counters.size()>;

  MemoryCollector(const IMemoryStatus& memory, std::unique_ptr<ICounterQuery> query,
                  Indices indices)
      : memory_(&memory), query_(std::move(query)), indices_(indices) {}

  [[nodiscard]] std::string_view name() const override { return "memory"; }
  [[nodiscard]] std::chrono::seconds interval() const override { return std::chrono::seconds{1}; }

  [[nodiscard]] Result<std::vector<Measurement>> collect(WallTime /*now*/,
                                                         MonoTime /*monotonic*/) override {
    const Result<MemoryStatus> status = memory_->read();
    if (!status) {
      return tl::unexpected(status.error());
    }
    if (Result<void> collected = query_->collect(); !collected) {
      return tl::unexpected(collected.error());
    }
    std::array<std::optional<double>, memory_counters.size()> counted{};
    for (std::size_t counter = 0; counter < counted.size(); ++counter) {
      Result<std::optional<double>> value = scalar(*query_, indices_.at(counter));
      if (!value) {
        return tl::unexpected(value.error());
      }
      counted.at(counter) = *value;
    }

    std::vector<Measurement> measurements;
    const auto add = [&](std::string_view name, double value) {
      measurements.push_back(measure(name, std::string(metric::system), value));
    };
    const auto bytes = [](Bytes value) { return static_cast<double>(value.count()); };
    add(metric::memory_total, bytes(status->total));
    add(metric::memory_available, bytes(status->available));
    add(metric::memory_commit, bytes(status->commit));
    add(metric::memory_commit_limit, bytes(status->commit_limit));
    add(metric::memory_pool_paged, bytes(status->pool_paged));
    add(metric::memory_pool_nonpaged, bytes(status->pool_nonpaged));
    if (counted[0] && counted[1] && counted[2]) {
      add(metric::memory_standby, *counted[0] + *counted[1] + *counted[2]);
    }
    if (counted[3]) {
      add(metric::memory_modified, *counted[3]);
    }
    if (counted[4]) {
      add(metric::memory_hard_faults, *counted[4]);
    }
    return measurements;
  }

 private:
  const IMemoryStatus* memory_;
  std::unique_ptr<ICounterQuery> query_;
  Indices indices_;
};

}  // namespace

Result<std::unique_ptr<ICollector>> make_cpu_collector(IPerfCounters& counters) {
  return make_counter_collector(counters, "cpu", cpu_counters, &cpu_entity);
}

Result<std::unique_ptr<ICollector>> make_disk_collector(IPerfCounters& counters) {
  return make_counter_collector(counters, "disks", disk_counters, &disk_entity);
}

Result<std::unique_ptr<ICollector>> make_memory_collector(const IMemoryStatus& memory,
                                                          IPerfCounters& counters) {
  Result<std::unique_ptr<ICounterQuery>> query = counters.open();
  if (!query) {
    return tl::unexpected(query.error());
  }
  MemoryCollector::Indices indices{};
  for (std::size_t counter = 0; counter < indices.size(); ++counter) {
    const Result<std::size_t> index = add_counter(**query, memory_counters.at(counter));
    if (!index) {
      return tl::unexpected(index.error());
    }
    indices.at(counter) = *index;
  }
  return std::make_unique<MemoryCollector>(memory, std::move(*query), indices);
}

}  // namespace myclean::telemetry
