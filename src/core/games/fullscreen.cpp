#include "core/games/fullscreen.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::games {

namespace {

constexpr std::array<std::string_view, 7> state_names = {
    "not_present", "busy", "running_d3d_full_screen", "presentation_mode", "accepts_notifications",
    "quiet_time",  "app"};

struct Engine3d {
  std::uint32_t pid = 0;
  std::string_view adapter;  // "luid_0x00000000_0x0001DEDF"
};

// "pid_1234_luid_0x00000000_0x0001DEDF_phys_0_eng_12_engtype_3D" -> 1234 and its LUID; nullopt
// for a malformed name or another engine type.
std::optional<Engine3d> parse_3d_instance(std::string_view name) {
  constexpr std::string_view pid_prefix = "pid_";
  constexpr std::string_view luid_marker = "_luid_";
  constexpr std::string_view phys_marker = "_phys_";
  constexpr std::string_view type_marker = "_engtype_";
  if (!name.starts_with(pid_prefix)) {
    return std::nullopt;
  }
  const std::size_t luid = name.find(luid_marker);
  const std::size_t phys = name.find(phys_marker);
  const std::size_t type = name.find(type_marker);
  if (luid == std::string_view::npos || phys == std::string_view::npos ||
      type == std::string_view::npos || luid >= phys || phys >= type ||
      name.substr(type + type_marker.size()) != "3D") {
    return std::nullopt;
  }
  const std::string_view digits = name.substr(pid_prefix.size(), luid - pid_prefix.size());
  constexpr std::size_t max_digits = 10;
  if (digits.empty() || digits.size() > max_digits) {
    return std::nullopt;
  }
  std::uint64_t pid = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    pid = (pid * 10) + static_cast<std::uint64_t>(digit - '0');
  }
  if (pid > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return Engine3d{.pid = static_cast<std::uint32_t>(pid),
                  .adapter = name.substr(luid + 1, phys - luid - 1)};
}

}  // namespace

std::string_view to_string(NotificationState state) {
  return detail::enum_name(state_names, state);
}

std::optional<NotificationState> parse_notification_state(std::string_view text) {
  return detail::enum_from_name<NotificationState>(state_names, text);
}

bool fullscreen_state(NotificationState state) {
  return state == NotificationState::busy || state == NotificationState::running_d3d_full_screen;
}

std::map<std::uint32_t, double> gpu_3d_by_pid(std::span<const telemetry::CounterValue> engines) {
  std::map<std::pair<std::uint32_t, std::string_view>, double> per_adapter;
  for (const telemetry::CounterValue& value : engines) {
    if (const std::optional<Engine3d> engine = parse_3d_instance(value.instance)) {
      per_adapter[{engine->pid, engine->adapter}] += std::max(value.value, 0.0);
    }
  }
  std::map<std::uint32_t, double> by_pid;
  for (const auto& [key, percent] : per_adapter) {
    double& busiest = by_pid[key.first];
    busiest = std::max(busiest, std::min(percent, 100.0));
  }
  return by_pid;
}

Result<Gpu3dSampler> Gpu3dSampler::open(telemetry::IPerfCounters& counters) {
  Result<std::unique_ptr<telemetry::ICounterQuery>> query = counters.open();
  if (!query) {
    return tl::unexpected(query.error());
  }
  const Result<std::size_t> index = (*query)->add(gpu_engine_counter);
  if (!index) {
    return fail(index.error().code, std::string(gpu_engine_counter) + ": " + index.error().message);
  }
  return Gpu3dSampler{std::move(*query), *index};
}

Result<std::map<std::uint32_t, double>> Gpu3dSampler::sample() {
  if (Result<void> collected = query_->collect(); !collected) {
    return tl::unexpected(collected.error());
  }
  const Result<std::vector<telemetry::CounterValue>> values = query_->values(index_);
  if (!values) {
    return tl::unexpected(values.error());
  }
  return gpu_3d_by_pid(*values);
}

FullscreenGpuDetector::FullscreenGpuDetector(FullscreenGpuPolicy policy, const os::IClock& clock)
    : policy_(policy), clock_(&clock) {}

std::optional<std::uint32_t> FullscreenGpuDetector::candidate() const {
  if (candidate_ && detected(*candidate_)) {
    return std::nullopt;
  }
  return candidate_;
}

std::optional<std::uint32_t> FullscreenGpuDetector::observe(
    const FullscreenObservation& observation) {
  if (!fullscreen_state(observation.state) || observation.foreground_pid == 0) {
    reset();
    return std::nullopt;
  }
  const MonoTime now = clock_->monotonic();
  if (candidate_ != observation.foreground_pid) {
    reset();
    candidate_ = observation.foreground_pid;
    since_ = now;
  }
  if (detected(*candidate_)) {
    return std::nullopt;
  }
  if (observation.gpu_3d_percent) {
    samples_.emplace_back(now, *observation.gpu_3d_percent);
  }
  while (!samples_.empty() && samples_.front().first < now - policy_.window) {
    samples_.pop_front();
  }
  if (now - since_ < policy_.window || samples_.empty() || samples_.size() < policy_.min_samples) {
    return std::nullopt;
  }
  double total = 0.0;
  for (const auto& [time, percent] : samples_) {
    total += percent;
  }
  if (total / static_cast<double>(samples_.size()) < policy_.min_gpu_percent) {
    return std::nullopt;
  }
  detected_.insert(*candidate_);
  samples_.clear();
  return candidate_;
}

void FullscreenGpuDetector::forget(std::uint32_t pid) {
  detected_.erase(pid);
  if (candidate_ == pid) {
    reset();
  }
}

void FullscreenGpuDetector::reset() {
  candidate_.reset();
  samples_.clear();
}

}  // namespace myclean::games
