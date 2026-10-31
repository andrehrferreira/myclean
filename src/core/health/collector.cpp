#include "core/health/collector.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/history.hpp"
#include "core/health/reading.hpp"
#include "core/health/source.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"

namespace myclean::health {

HealthCollector::HealthCollector(const IDiskHealthSource& source, store::Store& store,
                                 DriveSpecs specs, IAlertSink& alerts, const os::IClock& clock)
    : source_(&source),
      history_(store),
      specs_(std::move(specs)),
      alerts_(&alerts),
      clock_(&clock) {}

Result<std::unique_ptr<HealthCollector>> HealthCollector::create(const IDiskHealthSource& source,
                                                                 store::Store& store,
                                                                 DriveSpecs specs,
                                                                 IAlertSink& alerts,
                                                                 const os::IClock& clock) {
  std::unique_ptr<HealthCollector> collector(
      new HealthCollector(source, store, std::move(specs), alerts, clock));
  return collector->history_.add_retention().transform([&] { return std::move(collector); });
}

Result<std::vector<telemetry::Measurement>> HealthCollector::collect(WallTime now,
                                                                     MonoTime /*monotonic*/) {
  const Result<std::vector<DriveProbe>> probes = source_->probe();
  if (!probes) {
    return tl::unexpected(probes.error());
  }
  std::optional<Error> first_error;
  const auto keep = [&](const Error& error) {
    if (!first_error) {
      first_error = error;
    }
  };

  HealthView view{.time = now};
  std::vector<telemetry::Measurement> measurements;
  for (const DriveProbe& probe : *probes) {
    HealthReading reading = interpret(probe);
    const DriveSpec* spec = specs_.find(reading.model);

    std::optional<double> rate;
    if (reading.bytes_written) {
      const Result<std::optional<WrittenAt>> first =
          history_.first_written_since(reading.drive, now - rate_window);
      if (!first) {
        keep(first.error());
      } else if (*first) {
        rate = write_rate_tb_per_year(**first, {.time = now, .bytes = *reading.bytes_written});
      }
    }
    Assessment assessment = assess(reading, spec);
    alerts_->report(alert_readings(reading, assessment));
    Endurance endurance = project_endurance(reading.bytes_written,
                                            spec != nullptr ? spec->rated_tbw : std::nullopt, rate);

    if (const Result<void> saved = history_.save(reading, assessment.status, now, clock_->zone());
        !saved) {
      keep(saved.error());
    }
    if (reading.temperature_c) {
      measurements.push_back(telemetry::measure(metric_disk_temperature,
                                                std::to_string(reading.disk_number),
                                                static_cast<double>(*reading.temperature_c)));
    }
    view.drives.push_back(DriveHealth{.reading = std::move(reading),
                                      .assessment = std::move(assessment),
                                      .endurance = std::move(endurance)});
  }
  std::ranges::sort(view.drives, {},
                    [](const DriveHealth& drive) { return drive.reading.disk_number; });
  latest_.publish(std::move(view));
  if (first_error) {
    return tl::unexpected(*first_error);
  }
  return measurements;
}

Result<std::vector<Snapshot>> HealthCollector::history(std::string_view drive, WallTime from,
                                                       WallTime to) const {
  return history_.history(drive, from, to);
}

}  // namespace myclean::health
