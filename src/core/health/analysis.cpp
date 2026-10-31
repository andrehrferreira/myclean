#include "core/health/analysis.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/alerts/engine.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/reading.hpp"
#include "core/model/enum_names.hpp"

namespace myclean::health {

namespace {

constexpr std::array<std::string_view, 4> status_names{"good", "warning", "critical", "unknown"};

constexpr double bytes_per_tb = 1e12;
constexpr std::int64_t full_life_percent = 100;

// NVMe critical warning bits (NVMe Base Specification, log page 02h byte 0).
constexpr std::array<std::string_view, 5> critical_warning_bits{
    "available spare below threshold", "temperature outside its thresholds",
    "reliability degraded by media errors", "media placed in read-only mode",
    "volatile memory backup failed"};

std::string drive_name(const HealthReading& reading) {
  return std::format("{} (disk {})", reading.model, reading.disk_number);
}

int rank(Status status) {
  switch (status) {
    case Status::warning:
      return 1;
    case Status::critical:
      return 2;
    case Status::good:
    case Status::unknown:
      break;
  }
  return 0;
}

std::string decode_critical_warning(std::int64_t bits) {
  std::string names;
  for (std::size_t bit = 0; bit < critical_warning_bits.size(); ++bit) {
    if ((static_cast<std::uint64_t>(bits) & (1ULL << bit)) != 0) {
      names += names.empty() ? "" : ", ";
      names += critical_warning_bits.at(bit);
    }
  }
  return names.empty() ? "reserved bits" : names;
}

struct CountedFinding {
  const std::optional<std::int64_t>* value;
  std::string_view what;
};

}  // namespace

std::string_view to_string(Status status) {
  return detail::enum_name(status_names, status);
}

std::optional<Status> parse_status(std::string_view text) {
  return detail::enum_from_name<Status>(status_names, text);
}

Assessment assess(const HealthReading& reading, const DriveSpec* spec) {
  Assessment result;
  if (spec != nullptr && spec->max_temperature_c) {
    result.temperature_limit_c = spec->max_temperature_c;
  } else {
    result.temperature_note = "no rated limit known for " + reading.model;
  }
  if (reading.source == Source::none) {
    result.reasons.push_back(reading.problem.empty() ? "health could not be read"
                                                     : reading.problem);
    return result;
  }

  std::vector<std::string> critical;
  if (reading.predicts_failure.value_or(false)) {
    critical.emplace_back("the drive predicts its own failure (SMART)");
  }
  if (reading.critical_warning.value_or(0) != 0) {
    critical.push_back(std::format("NVMe critical warning 0x{:02x}: {}", *reading.critical_warning,
                                   decode_critical_warning(*reading.critical_warning)));
  }
  if (reading.available_spare && reading.spare_threshold &&
      *reading.available_spare < *reading.spare_threshold) {
    critical.push_back(std::format("available spare {} % is below its threshold {} %",
                                   *reading.available_spare, *reading.spare_threshold));
  }
  if (reading.percentage_used.value_or(0) >= full_life_percent) {
    critical.push_back(std::format("{} % of its rated life is used", *reading.percentage_used));
  }

  std::vector<std::string> warning;
  for (const CountedFinding& finding :
       {CountedFinding{.value = &reading.media_errors, .what = "media errors"},
        CountedFinding{.value = &reading.reallocated_sectors, .what = "reallocated sectors"},
        CountedFinding{.value = &reading.pending_sectors, .what = "pending sectors"},
        CountedFinding{.value = &reading.uncorrectable_sectors, .what = "uncorrectable sectors"},
        CountedFinding{.value = &reading.read_errors_uncorrected,
                       .what = "uncorrected read errors"},
        CountedFinding{.value = &reading.write_errors_uncorrected,
                       .what = "uncorrected write errors"}}) {
    if (finding.value->value_or(0) > 0) {
      warning.push_back(std::format("{} {}", **finding.value, finding.what));
    }
  }
  if (reading.temperature_c && result.temperature_limit_c &&
      *reading.temperature_c > *result.temperature_limit_c) {
    warning.push_back(std::format("{} °C is above the rated {} °C", *reading.temperature_c,
                                  *result.temperature_limit_c));
  }

  result.status = Status::good;
  if (!critical.empty()) {
    result.status = Status::critical;
  } else if (!warning.empty()) {
    result.status = Status::warning;
  }
  result.reasons = std::move(critical);
  result.reasons.insert(result.reasons.end(), warning.begin(), warning.end());
  return result;
}

std::vector<alerts::Reading> alert_readings(const HealthReading& reading,
                                            const Assessment& assessment) {
  const std::string name = drive_name(reading);
  std::vector<alerts::Reading> facts;
  const auto add = [&](std::string_view metric, double value, std::string detail) {
    facts.push_back(alerts::Reading{.metric = std::string(metric),
                                    .entity = reading.drive,
                                    .value = value,
                                    .detail = std::move(detail)});
  };
  struct Counter {
    std::string_view metric;
    const std::optional<std::int64_t>* value;
  };
  for (const Counter& counter :
       {Counter{.metric = metric_media_errors, .value = &reading.media_errors},
        Counter{.metric = metric_reallocated_sectors, .value = &reading.reallocated_sectors},
        Counter{.metric = metric_pending_sectors, .value = &reading.pending_sectors}}) {
    if (const std::optional<std::int64_t>& value = *counter.value) {
      add(counter.metric, static_cast<double>(*value), name);
    }
  }
  if (reading.temperature_c && assessment.temperature_limit_c) {
    add(metric_temperature_over_limit,
        static_cast<double>(*reading.temperature_c - *assessment.temperature_limit_c),
        std::format("{}: {} °C, rated limit {} °C", name, *reading.temperature_c,
                    *assessment.temperature_limit_c));
  }
  if (assessment.status != Status::unknown) {
    std::string reasons;
    for (const std::string& reason : assessment.reasons) {
      reasons += reasons.empty() ? " (" : "; ";
      reasons += reason;
    }
    add(metric_health_status, static_cast<double>(rank(assessment.status)),
        std::format("{}: {}{}", name, to_string(assessment.status),
                    reasons.empty() ? "" : reasons + ")"));
  }
  return facts;
}

std::optional<double> write_rate_tb_per_year(const WrittenAt& earlier, const WrittenAt& later) {
  const auto span = later.time - earlier.time;
  if (span < minimum_rate_window || later.bytes < earlier.bytes) {
    return std::nullopt;
  }
  const double years = std::chrono::duration<double, std::ratio<31'556'952>>(span).count();
  return static_cast<double>(later.bytes - earlier.bytes) / bytes_per_tb / years;
}

Endurance project_endurance(std::optional<std::int64_t> bytes_written,
                            std::optional<double> rated_tbw, std::optional<double> tb_per_year) {
  Endurance result{.rated_tbw = rated_tbw, .tb_per_year = tb_per_year};
  if (!bytes_written) {
    result.unavailable = "the drive does not report the data written";
    return result;
  }
  result.written_tb = static_cast<double>(*bytes_written) / bytes_per_tb;
  if (!rated_tbw) {
    result.unavailable = "projection unavailable: the rated TBW of this model is unknown";
    return result;
  }
  result.used_percent = *result.written_tb / *rated_tbw * 100.0;
  const double left_tb = std::max(0.0, *rated_tbw - *result.written_tb);
  if (left_tb == 0.0) {
    result.years_left = 0.0;
  } else if (!tb_per_year) {
    result.unavailable =
        std::format("projection unavailable: the write rate needs {} days of history",
                    minimum_rate_window.count());
  } else if (*tb_per_year <= 0.0) {
    result.unavailable = "projection unavailable: nothing was written in the measured period";
  } else {
    result.years_left = left_tb / *tb_per_year;
  }
  return result;
}

}  // namespace myclean::health
