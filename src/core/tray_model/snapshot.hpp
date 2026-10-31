#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/series.hpp"
#include "core/model/units.hpp"

namespace myclean::tray_model {

// The newest sample of every series the service reports (metrics.latest).
class MetricsSnapshot {
 public:
  using Entry = std::pair<SeriesKey, Sample>;

  // Adds the sample of `key`, or replaces the one it had.
  void set(SeriesKey key, Sample sample);

  // Every series, ordered by key.
  [[nodiscard]] const std::vector<Entry>& values() const { return values_; }
  [[nodiscard]] std::optional<double> value(std::string_view metric, std::string_view entity) const;
  // The entities that have a sample of `metric`, in natural order ("2" before "10", "0,9" before
  // "0,10").
  [[nodiscard]] std::vector<std::string> entities(std::string_view metric) const;

 private:
  std::vector<Entry> values_;  // sorted by key; a vector, so that moving a snapshot never throws
};

// Less-than over entity names that compares runs of digits by value.
[[nodiscard]] bool natural_less(std::string_view left, std::string_view right);

enum class FillState { insufficient_data, not_filling, filling };

struct VolumeRow {
  std::string name;  // the first mount point as a series entity: "C:", "D:\mnt\data"
  Bytes size;
  Bytes free;
  bool system = false;  // the volume Windows runs from
  FillState fill = FillState::insufficient_data;
  std::optional<double> days_until_full;  // set only while filling
  std::vector<std::string> disks;         // physical disk numbers, as disk.* entities
};

struct ProcessRow {
  std::uint32_t pid = 0;
  std::string image;
  double cpu_percent = 0.0;  // of all logical processors
  Bytes private_working_set;
  Bytes private_bytes;
};

// A process tree: the root and its members, with totals.
struct ProcessTreeRow {
  std::vector<ProcessRow> members;  // the root first; never empty
  double cpu_percent = 0.0;
  Bytes private_working_set;
  Bytes private_bytes;
  Bytes working_set;
};

enum class AlertSeverity { info, warning, critical };

// The active (not snoozed) alerts, when the service reports them.
struct AlertSummary {
  int active = 0;
  std::optional<AlertSeverity> worst;
};

}  // namespace myclean::tray_model
