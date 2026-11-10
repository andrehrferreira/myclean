#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/impact.hpp"
#include "core/store/store.hpp"

// Boot and logon time tracking (startup spec, "Boot and Logon Time Tracking"): the duration of
// each boot and of the logon it led to, as Windows measured them (Diagnostics-Performance event
// 100, see BootRecord), kept in table boot_time so that the 30-day median survives the event
// log's rollover; and the pieces of the boot view: the logon MyClean measured for a boot, whose
// logon_impact rows are the contributors, and the logons before and after a startup change.
namespace myclean::startup {

// The span the median the latest boot is compared with covers.
inline constexpr std::chrono::days median_span{30};
// How many boots on each side of a startup change its before/after medians are of.
inline constexpr std::size_t change_boots = 5;

// Stores `boots` in table boot_time (impact_migrations, version 2); a boot stored before (same
// start) is replaced.
[[nodiscard]] Result<void> record_boots(store::Store& store, std::span<const BootRecord> boots);
// Reads the events of `log` and stores the boots among them (record_boots).
[[nodiscard]] Result<void> record_boot_events(store::Store& store, const IDiagnosticsLog& log);
// Every boot stored, oldest first.
[[nodiscard]] Result<std::vector<BootRecord>> stored_boots(store::Store& store);
// Every logon LogonRecorder measured (table logon_recorded), oldest first.
[[nodiscard]] Result<std::vector<WallTime>> stored_logons(store::Store& store);
// The use of each program in the first 2 minutes of the logon at `logon`, by image.
[[nodiscard]] Result<std::vector<ImageImpact>> stored_logon_impact(store::Store& store,
                                                                   WallTime logon);

// The logon MyClean measured for boot `index` of `boots` (oldest first): the first of `logons`
// (oldest first) at or after its start and before the next boot's start; nullopt when none.
[[nodiscard]] std::optional<WallTime> boot_logon(std::span<const BootRecord> boots,
                                                 std::size_t index,
                                                 std::span<const WallTime> logons);

// `impact` ordered as contributors: most disk bytes first, then most CPU, then by image.
[[nodiscard]] std::vector<ImageImpact> by_impact(std::vector<ImageImpact> impact);

struct DurationMedian {
  std::chrono::milliseconds boot{};
  std::chrono::milliseconds logon{};
  std::size_t boots = 0;  // how many boots the medians are of

  friend bool operator==(const DurationMedian&, const DurationMedian&) = default;
};

// The medians of the boot and logon durations of `boots`; nullopt when it is empty.
[[nodiscard]] std::optional<DurationMedian> median_of(std::span<const BootRecord> boots);

// Of `boots` (oldest first): the latest `count` that started before `when`, and the first
// `count` that started at or after it.
[[nodiscard]] std::span<const BootRecord> boots_before(std::span<const BootRecord> boots,
                                                       WallTime when, std::size_t count);
[[nodiscard]] std::span<const BootRecord> boots_after(std::span<const BootRecord> boots,
                                                      WallTime when, std::size_t count);

}  // namespace myclean::startup
