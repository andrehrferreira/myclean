#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/alerts/engine.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/reading.hpp"
#include "core/model/units.hpp"

// Deterministic judgments on health readings: the drive's status, the facts it reports to the
// alert engine, and the write-endurance projection.
namespace myclean::health {

enum class Status { good, warning, critical, unknown };

[[nodiscard]] std::string_view to_string(Status status);
[[nodiscard]] std::optional<Status> parse_status(std::string_view text);

struct Assessment {
  Status status = Status::unknown;
  std::vector<std::string> reasons;  // one line per finding; empty when good
  // The model's rated temperature limit, or why there is none ("no rated limit known").
  std::optional<std::int64_t> temperature_limit_c;
  std::string temperature_note;
};

// The status of `reading`, judged against `spec` (null when the model is not listed):
//   unknown   nothing could be read (source none)
//   critical  the drive predicts its own failure or sets an NVMe critical warning, its
//             available spare is below its threshold, or it has used 100 % of its rated life
//   warning   any media error, reallocated, pending or uncorrectable sector, any uncorrected
//             read or write error, or a temperature above the model's rated limit
//   good      otherwise
[[nodiscard]] Assessment assess(const HealthReading& reading, const DriveSpec* spec);

// The facts every health reading reports to the alert engine (core/alerts), entity
// HealthReading::drive: the stable identity, so that a change of disk numbers never compares two
// different drives. The engine's rules in data/alerts.toml decide what alerts:
//   disk.media_errors, disk.reallocated_sectors, disk.pending_sectors   the counters, for
//       change rules ("increase": any new error or sector alerts, even below every threshold)
//   disk.temperature_over_limit   °C above the model's rated limit (negative when below); only
//       when the model has a limit, so an unknown model never raises a temperature alert
//   disk.health_status            0 good, 1 warning, 2 critical; not reported while unknown
inline constexpr std::string_view metric_media_errors = "disk.media_errors";
inline constexpr std::string_view metric_reallocated_sectors = "disk.reallocated_sectors";
inline constexpr std::string_view metric_pending_sectors = "disk.pending_sectors";
inline constexpr std::string_view metric_temperature_over_limit = "disk.temperature_over_limit";
inline constexpr std::string_view metric_health_status = "disk.health_status";

// The alert facts of `reading`, whose assessment is `assessment`. Each carries the drive's name
// and disk number in its detail, plus the temperature and limit or the status reasons.
[[nodiscard]] std::vector<alerts::Reading> alert_readings(const HealthReading& reading,
                                                          const Assessment& assessment);

// Receives the alert facts of every health read; the service forwards them to
// alerts::AlertEngine::report and delivers the toasts it returns.
class IAlertSink {
 public:
  virtual ~IAlertSink() = default;
  IAlertSink(const IAlertSink&) = delete;
  IAlertSink& operator=(const IAlertSink&) = delete;
  IAlertSink(IAlertSink&&) = delete;
  IAlertSink& operator=(IAlertSink&&) = delete;

  virtual void report(std::span<const alerts::Reading> readings) = 0;

 protected:
  IAlertSink() = default;
};
// Data written at one point in time.
struct WrittenAt {
  WallTime time;
  std::int64_t bytes = 0;
};

// The write rate needs at least this much history to be meaningful.
inline constexpr std::chrono::days minimum_rate_window{7};

// Terabytes (10^12 bytes) written per year between two points; nullopt when they are less than
// minimum_rate_window apart or the counter went backwards.
[[nodiscard]] std::optional<double> write_rate_tb_per_year(const WrittenAt& earlier,
                                                           const WrittenAt& later);

struct Endurance {
  std::optional<double> written_tb;
  std::optional<double> rated_tbw;
  std::optional<double> used_percent;  // of the rated TBW
  std::optional<double> tb_per_year;
  std::optional<double> years_left;  // at tb_per_year; 0 once the rating is used up
  // Why the projection is incomplete; empty when years_left is known.
  std::string unavailable;
};

[[nodiscard]] Endurance project_endurance(std::optional<std::int64_t> bytes_written,
                                          std::optional<double> rated_tbw,
                                          std::optional<double> tb_per_year);

}  // namespace myclean::health
