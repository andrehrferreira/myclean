#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/sampler.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

// The stored use of each program (UsageSampler keys): the private-bytes trend the leak detector
// reads and the hourly totals behind the background-app cost report.
namespace myclean::resident {

// Component "resident":
//   resident_fine  average private bytes per key and 5-minute bucket (the leak detector's trend)
//   resident_hour  per key and hour: minutes running, CPU seconds, the sum and peak of the RAM
//                  of each minute sample, disk and network bytes (the cost report)
[[nodiscard]] const store::MigrationSet& resident_migrations();

// resident_fine is kept fine_span, resident_hour hour_span.
[[nodiscard]] std::vector<store::RetentionRule> resident_retention();

inline constexpr std::chrono::minutes bucket{5};
inline constexpr std::chrono::hours fine_span{48};
inline constexpr std::chrono::days hour_span{35};

// One 5-minute bucket of a key: when it starts and the average private bytes of its samples.
struct Point {
  WallTime time;
  Bytes private_bytes;
  int samples = 0;

  friend bool operator==(const Point&, const Point&) = default;
};

// Collects the minute samples of every key into 5-minute buckets and hours, writes each one
// once it is over (Store::enqueue), and keeps the buckets of the last fine_span in memory. A
// bucket or hour written early (flush() at a stop) is added to when more samples of it come
// after a restart. Not thread-safe.
class History {
 public:
  // `store` (opened with resident_migrations()) must outlive the history.
  explicit History(store::Store& store) : store_(&store) {}

  // Reads the stored buckets of the last fine_span before `now`.
  [[nodiscard]] Result<void> load(WallTime now);

  // Adds the use of the minute sampled at `time`; the bucket and hour before it are written when
  // `time` is past them. Returns true when a bucket was closed.
  bool add(WallTime time, std::span<const MinuteUse> use);

  // Writes the open bucket and hour now (at a stop).
  void flush();

  // The buckets of each key in the last fine_span, oldest first; the open bucket is not in them.
  [[nodiscard]] const std::map<std::string, std::vector<Point>, std::less<>>& trends() const {
    return trends_;
  }

 private:
  struct BucketSum {
    std::uint64_t private_sum = 0;
    int samples = 0;
  };
  struct HourSum {
    std::int64_t minutes = 0;
    double cpu_seconds = 0.0;
    std::uint64_t ram_sum = 0;
    std::uint64_t ram_peak = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t net_bytes = 0;
  };

  void close_bucket();
  void close_hour();

  store::Store* store_;
  std::optional<WallTime> bucket_start_;
  std::map<std::string, BucketSum, std::less<>> bucket_;
  std::optional<WallTime> hour_start_;
  std::map<std::string, HourSum, std::less<>> hour_;
  std::map<std::string, std::vector<Point>, std::less<>> trends_;
};

// What one program cost over a time range.
struct Cost {
  std::string key;
  std::chrono::minutes running{};  // minute samples in which a tree of it ran
  double cpu_seconds = 0.0;
  Bytes ram_average;  // private working set, averaged over the minutes it ran
  Bytes ram_peak;     // the largest minute sample
  std::uint64_t disk_bytes = 0;
  std::uint64_t net_bytes = 0;

  friend bool operator==(const Cost&, const Cost&) = default;
};

// The stored hours starting at or after `from`, per key, for the keys that ran at least
// `min_running` in them (the resident ones); most CPU time first, then by key.
[[nodiscard]] Result<std::vector<Cost>> costs(store::Store& store, WallTime from,
                                              std::chrono::minutes min_running);

}  // namespace myclean::resident
