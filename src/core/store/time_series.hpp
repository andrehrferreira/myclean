#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "core/model/series.hpp"
#include "core/model/units.hpp"

namespace myclean::store {

// A summary of one series over one minute or one hour.
struct Aggregate {
  WallTime time;  // start of the minute or hour (UTC)
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::int64_t count = 0;  // number of samples summarized
};

struct ClosedMinute {
  SeriesKey key;
  Aggregate aggregate;
};

// The in-memory tier of the time series: the 1 s samples of the last hour of every series (the
// raw ring) and, per series, the minute currently being aggregated. Not thread-safe.
class SeriesBuffer {
 public:
  static constexpr std::chrono::hours raw_window{1};

  // Adds a sample to the ring and to its minute. A sample of a later minute closes the open
  // minute; a sample of a minute that is already closed reaches the ring only.
  void add(const SeriesKey& key, Sample sample);
  // Closes every open minute that ended at or before `cutoff`, then returns every minute closed
  // since the previous call.
  [[nodiscard]] std::vector<ClosedMinute> take_closed(WallTime cutoff);
  // The samples of `key` newer than `now` - 1 h, oldest first.
  [[nodiscard]] std::vector<Sample> raw(const SeriesKey& key, WallTime now) const;
  // Drops samples older than the window, and series left with no samples and no open minute.
  void evict(WallTime now);

 private:
  struct Bucket {
    WallTime minute;
    double min = 0.0;
    double max = 0.0;
    double sum = 0.0;
    std::int64_t count = 0;
  };
  struct Entry {
    std::deque<Sample> samples;  // oldest first
    std::optional<Bucket> open;
    std::optional<WallTime> closed_through;  // start of the last minute closed
  };

  void close(const SeriesKey& key, Entry& entry);

  std::map<SeriesKey, Entry> series_;
  std::vector<ClosedMinute> closed_;
};

}  // namespace myclean::store
