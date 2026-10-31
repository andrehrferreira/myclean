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

// The trust signal of a process image (processes.top, processes.explain): facts, not a verdict.
struct TrustRow {
  std::string status;  // "signed", "unsigned" or "invalid"
  std::optional<std::string> signer;
  std::string path;
  bool user_writable = false;  // the image is in a folder a standard user can write
  bool unrecognized = false;
  std::vector<std::string> reasons;  // why unrecognized
  std::optional<std::string> description;
  std::optional<std::string> category;
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
  std::optional<TrustRow> trust;  // of the root; nullopt when its image could not be read
};

// The knowledge-base entry of a process image.
struct KbRow {
  std::string description;
  std::string category;
  bool needed_at_startup = false;
  bool safe_to_remove = false;
};

// One running process explained (processes.explain).
struct Explanation {
  std::uint32_t pid = 0;
  std::string image;
  std::optional<std::string> path;
  double cpu_percent = 0.0;
  double memory_percent = 0.0;            // private working set, of physical memory
  std::optional<std::uint32_t> tree_pid;  // the root of its tree; nullopt when in none
  std::string tree_image;
  std::vector<ProcessRow> members;  // of its tree
  std::optional<TrustRow> trust;
  std::optional<KbRow> kb;
};

enum class AlertSeverity { info, warning, critical };

[[nodiscard]] std::string_view to_string(AlertSeverity severity);

// The active (not snoozed) alerts, when the service reports them.
struct AlertSummary {
  int active = 0;
  std::optional<AlertSeverity> worst;

  friend bool operator==(const AlertSummary&, const AlertSummary&) = default;
};

// One stored alert (alerts.list).
struct AlertRow {
  std::string rule;
  std::string entity;  // in token form, as the snooze and dismiss methods take it
  std::string title;
  AlertSeverity severity = AlertSeverity::warning;
  WallTime first;
  WallTime last;
  std::int64_t count = 0;
  bool active = false;
  std::optional<WallTime> snoozed_until;
  // When the service last decided this alert deserves a toast (after its rate limit and snooze).
  std::optional<WallTime> toast_at;
  std::string detail;
};

// The health of a drive, worst last.
enum class DriveStatus { unknown, good, warning, failing };

[[nodiscard]] std::string_view to_string(DriveStatus status);

// The newest health reading of one physical disk (disks.health). A nullopt counter is one the
// drive does not report.
struct DriveHealthRow {
  std::string key;
  std::string disk;  // the disk number, as the disk.* entities
  std::string model;
  std::string bus;
  std::string source;
  std::vector<std::string> volumes;
  DriveStatus status = DriveStatus::unknown;
  std::vector<std::string> reasons;
  bool readable = false;  // the drive's own health data (NVMe log, SMART) was read
  std::optional<std::string> unreadable_reason;
  std::optional<std::int64_t> temperature_c;
  std::optional<std::int64_t> temperature_limit_c;  // nullopt: no rated limit known
  std::optional<std::int64_t> percent_used;
  std::optional<std::int64_t> available_spare;
  std::optional<std::int64_t> media_errors;
  std::optional<std::int64_t> reallocated_sectors;
  std::optional<std::int64_t> pending_sectors;
  std::optional<std::int64_t> power_on_hours;
  std::optional<std::int64_t> data_written_bytes;
  std::string endurance_state;  // "projected", "rating_unknown", ...
  std::optional<double> years_left;
  std::optional<double> tbw_rated;
  std::optional<double> tbw_used_percent;
  std::string endurance_note;  // why there is no projection; empty when projected
};

// One copy/paste freeze of a Freezes group (input.summary).
struct FreezeRow {
  std::int64_t id = 0;
  std::string chord;  // "copy" or "paste"
  WallTime time;
  std::int64_t latency_ms = 0;
  std::optional<std::string> cause;  // the process named as the cause
  std::vector<std::string> evidence;
};

// The freezes of one mechanism and one process, newest group first.
struct FreezeGroupRow {
  std::string mechanism;
  std::optional<std::string> process;
  std::int64_t count = 0;
  WallTime latest;
  std::vector<FreezeRow> incidents;  // the newest of the group
};

}  // namespace myclean::tray_model
