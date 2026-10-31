#include "core/telemetry/process_collector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::telemetry {

namespace {

constexpr std::array<std::string_view, 5> launchers{"explorer.exe", "services.exe", "svchost.exe",
                                                    "wininit.exe", "winlogon.exe"};

bool equal_ignoring_case(std::string_view left, std::string_view right) {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  return std::ranges::equal(left, right, {}, lower, lower);
}

bool is_launcher(std::string_view image) {
  return std::ranges::any_of(
      launchers, [&](std::string_view name) { return equal_ignoring_case(image, name); });
}

void link_parents(std::vector<ProcessRow>& rows) {
  std::unordered_map<std::uint32_t, std::size_t> by_pid;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    by_pid.emplace(rows[index].info.pid, index);
  }
  for (ProcessRow& row : rows) {
    const auto found = by_pid.find(row.info.parent_pid);
    if (found == by_pid.end() || row.info.parent_pid == row.info.pid) {
      continue;
    }
    const ProcessInfo& parent = rows[found->second].info;
    if (parent.created < row.info.created && !is_launcher(parent.image)) {
      row.parent = found->second;
    }
  }
}

// Parent links always point to an older process, so they cannot form a cycle.
std::vector<ProcessTree> build_trees(const std::vector<ProcessRow>& rows) {
  std::vector<std::vector<std::size_t>> children(rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    if (rows[index].parent) {
      children[*rows[index].parent].push_back(index);
    }
  }
  std::vector<ProcessTree> trees;
  for (std::size_t root = 0; root < rows.size(); ++root) {
    if (rows[root].parent) {
      continue;
    }
    ProcessTree tree;
    tree.members.push_back(root);
    for (std::size_t next = 0; next < tree.members.size(); ++next) {
      const std::vector<std::size_t>& below = children[tree.members[next]];
      tree.members.insert(tree.members.end(), below.begin(), below.end());
    }
    for (const std::size_t member : tree.members) {
      const ProcessRow& row = rows[member];
      tree.cpu_percent += row.cpu_percent;
      tree.private_bytes += row.info.private_bytes;
      tree.private_working_set += row.info.private_working_set;
      tree.working_set += row.info.working_set;
      tree.io_bytes_per_second += row.io_bytes_per_second;
      tree.handles += row.info.handles;
      tree.threads += row.info.threads;
    }
    trees.push_back(std::move(tree));
  }
  std::ranges::stable_sort(trees, std::ranges::greater{}, &ProcessTree::cpu_percent);
  return trees;
}

}  // namespace

ProcessCollector::ProcessCollector(IProcessTable& table) : table_(&table) {}

Result<std::vector<Measurement>> ProcessCollector::collect(WallTime now, MonoTime monotonic) {
  Result<std::vector<ProcessInfo>> snapshot = table_->snapshot();
  if (!snapshot) {
    return tl::unexpected(snapshot.error());
  }

  using Seconds = std::chrono::duration<double>;
  const double elapsed =
      previous_time_ ? std::chrono::duration_cast<Seconds>(monotonic - *previous_time_).count()
                     : 0.0;
  const double processors = static_cast<double>(std::max(1U, table_->logical_processors()));

  ProcessView view{.time = now};
  view.rows.reserve(snapshot->size());
  std::map<Key, Counters> current;
  for (ProcessInfo& info : *snapshot) {
    const Key key{.pid = info.pid, .created = info.created};
    current.emplace(key, Counters{.cpu_time = info.cpu_time, .io_bytes = info.io_bytes});
    ProcessRow row{.info = std::move(info)};
    const auto before = previous_.find(key);
    if (elapsed > 0.0 && before != previous_.end()) {
      const double cpu =
          std::chrono::duration_cast<Seconds>(row.info.cpu_time - before->second.cpu_time).count();
      row.cpu_percent = std::clamp(cpu / (elapsed * processors) * 100.0, 0.0, 100.0);
      if (row.info.io_bytes >= before->second.io_bytes) {
        row.io_bytes_per_second =
            static_cast<double>(row.info.io_bytes - before->second.io_bytes) / elapsed;
      }
    }
    view.rows.push_back(std::move(row));
  }
  link_parents(view.rows);
  view.trees = build_trees(view.rows);
  previous_ = std::move(current);
  previous_time_ = monotonic;

  std::uint64_t threads = 0;
  std::uint64_t handles = 0;
  std::uint64_t ready = 0;
  std::vector<Measurement> measurements;
  for (const ProcessRow& row : view.rows) {
    threads += row.info.threads;
    handles += row.info.handles;
    ready += row.info.ready_threads;
    if (row.info.pid != 0 && row.info.image == "Memory Compression") {
      measurements.push_back(measure(metric::memory_compressed, std::string(metric::system),
                                     static_cast<double>(row.info.working_set.count())));
    }
  }
  measurements.push_back(measure(metric::process_count, std::string(metric::system),
                                 static_cast<double>(view.rows.size())));
  measurements.push_back(
      measure(metric::process_threads, std::string(metric::system), static_cast<double>(threads)));
  measurements.push_back(
      measure(metric::process_handles, std::string(metric::system), static_cast<double>(handles)));
  measurements.push_back(
      measure(metric::cpu_queue, std::string(metric::system), static_cast<double>(ready)));
  latest_.publish(std::move(view));
  return measurements;
}

}  // namespace myclean::telemetry
