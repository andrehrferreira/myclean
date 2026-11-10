#include "core/resident/sampler.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/units.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::resident {
namespace {

std::int64_t seconds_of(WallTime time) {
  return std::chrono::floor<std::chrono::seconds>(time).time_since_epoch().count();
}

// The entry of `key` in `use` (sorted by key), added when there is none.
MinuteUse& entry_of(std::vector<MinuteUse>& use, const std::string& key, bool running) {
  const auto at = std::ranges::lower_bound(use, key, {}, &MinuteUse::key);
  if (at != use.end() && at->key == key) {
    return *at;
  }
  return *use.insert(at, MinuteUse{.key = key, .running = running});
}

}  // namespace

std::string tree_key(std::string_view image) {
  const std::size_t slash = image.find_last_of("\\/");
  std::string key(slash == std::string_view::npos ? image : image.substr(slash + 1));
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return key;
}

void UsageSampler::observe(const telemetry::ProcessView& view) {
  memory_.clear();
  for (const telemetry::ProcessTree& tree : view.trees) {
    const telemetry::ProcessInfo& root = view.rows[tree.members.front()].info;
    if (root.pid == 0) {
      continue;  // the System Idle Process: its "CPU time" is the time nothing ran
    }
    const std::string key = tree_key(root.image);
    MinuteUse& memory = memory_[key];
    memory.key = key;
    Used& used = used_[key];
    for (const std::size_t member : tree.members) {
      const telemetry::ProcessInfo& info = view.rows[member].info;
      memory.private_bytes += info.private_bytes;
      memory.ram += info.private_working_set;
      const Instance instance{.pid = info.pid, .created = seconds_of(info.created)};
      if (const auto before = seen_.find(instance); before != seen_.end()) {
        used.cpu_seconds +=
            std::chrono::duration<double>(std::max(info.cpu_time - before->second.cpu, {})).count();
        used.disk_bytes +=
            info.io_bytes >= before->second.io_bytes ? info.io_bytes - before->second.io_bytes : 0;
      } else if (previous_ && info.created > *previous_) {
        used.cpu_seconds += std::chrono::duration<double>(info.cpu_time).count();
        used.disk_bytes += info.io_bytes;
      }
      seen_.insert_or_assign(
          instance,
          Seen{.key = key, .cpu = info.cpu_time, .io_bytes = info.io_bytes, .last = view.time});
    }
  }
  std::erase_if(seen_, [&](const auto& entry) { return view.time - entry.second.last > remember; });
  previous_ = view.time;
}

std::vector<MinuteUse> UsageSampler::take() {
  std::vector<MinuteUse> use;
  use.reserve(memory_.size());
  for (const auto& [key, memory] : memory_) {
    use.push_back(memory);
  }
  for (const auto& [key, used] : used_) {
    MinuteUse& entry = entry_of(use, key, false);
    entry.cpu_seconds = used.cpu_seconds;
    entry.disk_bytes = used.disk_bytes;
  }
  used_.clear();
  return use;
}

const UsageSampler::Seen* UsageSampler::find(std::uint32_t pid,
                                             std::optional<WallTime> created) const {
  if (created) {
    // The network history and the process table may round the creation time to neighbouring
    // seconds.
    const std::int64_t second = seconds_of(*created);
    for (const std::int64_t near : {second, second - 1, second + 1}) {
      if (const auto found = seen_.find({.pid = pid, .created = near}); found != seen_.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }
  // Without a creation time the PID alone decides, when only one known process had it.
  const Seen* only = nullptr;
  for (auto at =
           seen_.lower_bound({.pid = pid, .created = std::numeric_limits<std::int64_t>::min()});
       at != seen_.end() && at->first.pid == pid; ++at) {
    if (only != nullptr) {
      return nullptr;
    }
    only = &at->second;
  }
  return only;
}

void UsageSampler::attribute(std::span<const ProcessTraffic> traffic,
                             std::vector<MinuteUse>& use) const {
  for (const ProcessTraffic& process : traffic) {
    if (process.bytes == 0) {
      continue;
    }
    if (const Seen* seen = find(process.pid, process.created); seen != nullptr) {
      entry_of(use, seen->key, false).net_bytes += process.bytes;
    } else if (!process.image.empty()) {
      entry_of(use, tree_key(process.image), false).net_bytes += process.bytes;
    }
  }
}

}  // namespace myclean::resident
