#include "core/store/time_series.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>
#include <vector>

#include "core/model/series.hpp"
#include "core/model/units.hpp"

namespace myclean::store {

void SeriesBuffer::add(const SeriesKey& key, Sample sample) {
  Entry& entry = series_[key];
  const auto by_time = [](const Sample& left, const Sample& right) {
    return left.time < right.time;
  };
  entry.samples.insert(std::ranges::upper_bound(entry.samples, sample, by_time), sample);

  const WallTime minute = std::chrono::floor<std::chrono::minutes>(sample.time);
  if (entry.closed_through && minute <= *entry.closed_through) {
    return;
  }
  if (entry.open && entry.open->minute < minute) {
    close(key, entry);
  }
  if (!entry.open) {
    entry.open = Bucket{.minute = minute, .min = sample.value, .max = sample.value};
  }
  if (entry.open->minute == minute) {
    Bucket& bucket = *entry.open;
    bucket.min = std::min(bucket.min, sample.value);
    bucket.max = std::max(bucket.max, sample.value);
    bucket.sum += sample.value;
    ++bucket.count;
  }
}

std::vector<ClosedMinute> SeriesBuffer::take_closed(WallTime cutoff) {
  for (auto& [key, entry] : series_) {
    if (entry.open && entry.open->minute + std::chrono::minutes{1} <= cutoff) {
      close(key, entry);
    }
  }
  return std::exchange(closed_, {});
}

std::vector<Sample> SeriesBuffer::raw(const SeriesKey& key, WallTime now) const {
  const auto found = series_.find(key);
  if (found == series_.end()) {
    return {};
  }
  std::vector<Sample> samples;
  std::ranges::copy_if(found->second.samples, std::back_inserter(samples),
                       [&](const Sample& sample) { return sample.time > now - raw_window; });
  return samples;
}

void SeriesBuffer::evict(WallTime now) {
  for (auto it = series_.begin(); it != series_.end();) {
    std::deque<Sample>& samples = it->second.samples;
    while (!samples.empty() && samples.front().time <= now - raw_window) {
      samples.pop_front();
    }
    if (samples.empty() && !it->second.open) {
      it = series_.erase(it);
    } else {
      ++it;
    }
  }
}

void SeriesBuffer::close(const SeriesKey& key, Entry& entry) {
  const Bucket& bucket = *entry.open;
  closed_.push_back(
      ClosedMinute{.key = key,
                   .aggregate = Aggregate{.time = bucket.minute,
                                          .min = bucket.min,
                                          .max = bucket.max,
                                          .mean = bucket.sum / static_cast<double>(bucket.count),
                                          .count = bucket.count}});
  entry.closed_through = bucket.minute;
  entry.open.reset();
}

}  // namespace myclean::store
