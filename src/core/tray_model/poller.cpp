#include "core/tray_model/poller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

namespace {

std::optional<std::vector<ProcessTreeRow>> fetch_top(IServiceLink& link, ProcessSort sort) {
  const Result<nlohmann::json> reply =
      link.call(ipc::processes_top_method, processes_top_params(sort, Poller::top_trees));
  if (!reply) {
    return std::nullopt;
  }
  Result<std::vector<ProcessTreeRow>> trees = parse_processes_top(*reply);
  return trees ? std::optional(std::move(*trees)) : std::nullopt;
}

}  // namespace

PollResult Poller::poll(WallTime now, bool dashboard_open) {
  PollResult result;
  result.time = now;
  const Result<nlohmann::json> reply =
      link_->call(ipc::metrics_latest_method, nlohmann::json::object());
  if (!link_->reachable()) {
    result.error = reply ? std::string() : reply.error().message;
    return result;
  }
  if (reply) {
    if (Result<LatestReply> latest = parse_metrics_latest(*reply)) {
      fill_gaps(*latest, dashboard_open, result);
      if (dashboard_open) {
        poll_processors(latest->logical_processors, now, result);
      }
      result.latest = std::move(*latest);
    }
  }
  if (dashboard_open) {
    result.top_by_cpu = fetch_top(*link_, ProcessSort::cpu);
    result.top_by_memory = fetch_top(*link_, ProcessSort::memory);
  }
  // The service may have gone away after the first call.
  result.reachable = link_->reachable();
  if (!result.reachable) {
    result.error = "the connection to the service broke";
  }
  return result;
}

std::optional<Series> Poller::query(std::string_view metric, std::string_view entity,
                                    std::int64_t from, std::int64_t to) {
  const Result<nlohmann::json> reply =
      link_->call(ipc::metrics_query_method, metrics_query_params(metric, entity, from, to));
  if (!reply) {
    return std::nullopt;
  }
  Result<Series> series = parse_metrics_query(*reply);
  return series ? std::optional(std::move(*series)) : std::nullopt;
}

void Poller::fill_gaps(const LatestReply& latest, bool dashboard_open, PollResult& result) {
  for (const auto& [key, sample] : latest.metrics.values()) {
    const auto [previous, inserted] = newest_.try_emplace(key, sample.time);
    if (inserted || sample.time <= previous->second) {
      continue;
    }
    const WallTime last = std::exchange(previous->second, sample.time);
    const auto gap = sample.time - last;
    if (!dashboard_open || gap <= std::chrono::seconds{1} || gap > window_) {
      continue;
    }
    if (std::optional<Series> missed =
            query(key.metric, key.entity, unix_seconds(last) + 1, unix_seconds(sample.time))) {
      result.history.push_back(std::move(*missed));
    }
  }
}

void Poller::poll_processors(unsigned count, WallTime now, PollResult& result) {
  const std::int64_t to = unix_seconds(now) + 1;
  const std::int64_t oldest = unix_seconds(now) - window_.count();
  for (unsigned index = 0; index < count; ++index) {
    for (const std::string_view metric :
         {telemetry::metric::cpu_utility, telemetry::metric::cpu_frequency}) {
      const SeriesKey key{.metric = std::string(metric), .entity = processor_entity(index)};
      const std::int64_t from = std::max(next_from_[key], oldest);
      if (from >= to) {
        continue;
      }
      std::optional<Series> series = query(metric, key.entity, from, to);
      if (!series) {
        continue;
      }
      if (!series->samples.empty()) {
        next_from_[key] = unix_seconds(series->samples.back().time) + 1;
      }
      result.history.push_back(std::move(*series));
    }
  }
}

}  // namespace myclean::tray_model
