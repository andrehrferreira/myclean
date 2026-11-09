#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/startup.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

// Measured startup impact (startup spec, "Measured Startup Impact"): what each program used in
// the first 2 minutes after a logon, from MyClean's own process history, and the impact shown for
// each startup item.
namespace myclean::startup {

inline constexpr std::chrono::seconds logon_window{120};

// One process start, as the ETW history records it (events::proc_start).
struct Start {
  std::uint32_t pid = 0;
  WallTime time;
  std::uint32_t parent_pid = 0;
  std::optional<WallTime> parent_time;
  std::string image;  // full path or file name, as recorded
};

// What one process had used by the time of a process-table sample: totals since it started.
struct Usage {
  std::uint32_t pid = 0;
  WallTime created;
  std::string image;
  std::chrono::nanoseconds cpu{};
  std::uint64_t io_bytes = 0;  // read + write + other transfers, as Windows counts them
};

// The use of one program in a logon window.
struct ImageImpact {
  std::string image;  // lower-case file name ("discord.exe")
  double cpu_seconds = 0.0;
  std::uint64_t io_bytes = 0;

  friend bool operator==(const ImageImpact&, const ImageImpact&) = default;
};

// The lower-case file name of the program `command` runs: the quoted path, or the text up to the
// first ".exe" (paths with spaces and no quotes), or the first word.
[[nodiscard]] std::string command_image(std::string_view command);

// The use of each program in the window [logon, logon + logon_window), by image:
//  - a process started in the window counts for the image of the first process of its chain of
//    parents that started in the window (Discord.exe started by Update.exe counts for
//    update.exe, even after Update.exe exited), with its totals at `end`;
//  - a process that was already running at the logon (a service) counts for its own image, with
//    what it used between `baseline` (a sample taken right after the logon) and `end`; one
//    missing from `baseline` is left out.
// `starts`: the process starts recorded in the window; `end`: a sample taken at the window's
// end. Sorted by image.
[[nodiscard]] std::vector<ImageImpact> logon_impact(WallTime logon, std::span<const Start> starts,
                                                    std::span<const Usage> baseline,
                                                    std::span<const Usage> end);

// Where the impact shown for an item comes from.
enum class ImpactSource {
  myclean,      // "myclean": MyClean's own logon history
  diagnostics,  // "diagnostics": Windows' Diagnostics-Performance boot events (101)
};

[[nodiscard]] std::string_view to_string(ImpactSource source);

struct ItemImpact {
  ImpactSource source = ImpactSource::myclean;
  double cpu_seconds = 0.0;    // myclean: median over the logons recorded
  std::uint64_t io_bytes = 0;  // myclean: median over the logons recorded
  std::uint32_t logons = 0;    // myclean: how many logons the medians are of
  double delay_seconds = 0.0;  // diagnostics: the boot delay Windows attributed to it
};

// A Diagnostics-Performance event 101: an application Windows found to slow the boot.
struct BootDelay {
  std::string image;  // lower-case file name
  double delay_seconds = 0.0;
};

// The impact of each item of `items`, in order: from `history` (one ImageImpact list per logon
// recorded) when it has a logon, as the median of the item's image over every logon (a logon
// without the image counts as 0); otherwise from `delays` (the largest delay of the image);
// nullopt when neither knows the image.
[[nodiscard]] std::vector<std::optional<ItemImpact>> item_impacts(
    std::span<const Item> items, std::span<const std::vector<ImageImpact>> history,
    std::span<const BootDelay> delays);

// Table logon_impact: one row per logon and image.
[[nodiscard]] const store::MigrationSet& impact_migrations();

// Stores the impact of the logon at `logon`; a logon stored before is replaced.
[[nodiscard]] Result<void> record_logon(store::Store& store, WallTime logon,
                                        std::span<const ImageImpact> impact);
// Whether the logon at `logon` is stored.
[[nodiscard]] Result<bool> logon_recorded(store::Store& store, WallTime logon);
// The impact of the latest `count` logons stored, newest first.
[[nodiscard]] Result<std::vector<std::vector<ImageImpact>>> logon_history(store::Store& store,
                                                                          std::size_t count);

// The process starts stored in [from, to) (events::process_starts).
using StartSource = std::function<Result<std::vector<Start>>(WallTime from, WallTime to)>;

// Measures each logon of the owner once, from samples of the process table taken every second:
// a baseline at the first tick after the logon, the end sample once the window is over, and,
// `flush_delay` later (the process starts of the window are stored by then), the impact, stored
// with record_logon. A logon already stored, or first seen more than `baseline_grace` after it
// began (the service started late), is not measured.
class LogonRecorder {
 public:
  static constexpr std::chrono::seconds baseline_grace{15};
  static constexpr std::chrono::seconds flush_delay{15};

  // `store` (opened with impact_migrations()) must outlive the recorder.
  explicit LogonRecorder(store::Store& store) : store_(&store) {}

  // `logon`: when the owner's current logon began, nullopt while the owner is not logged on;
  // `sample`: the process table now.
  [[nodiscard]] Result<void> tick(WallTime now, std::optional<WallTime> logon,
                                  std::span<const Usage> sample, const StartSource& starts);

 private:
  store::Store* store_;
  std::optional<WallTime> logon_;  // the logon of the latest tick
  bool done_ = true;               // measured, or not measurable
  std::vector<Usage> baseline_;
  std::optional<std::vector<Usage>> end_;
};

}  // namespace myclean::startup
