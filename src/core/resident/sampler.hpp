#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/units.hpp"
#include "core/telemetry/process_collector.hpp"

// What each program's process trees use, minute by minute (optimizer spec, "Memory-Leak
// Detection", "Background-App Cost Report").
namespace myclean::resident {

// The program a process tree's use is recorded under: the lower-case file name of its root's
// image ("armourycrate.service.exe"), so that restarts and several trees of one program add up.
[[nodiscard]] std::string tree_key(std::string_view image);

// What the trees of one program used over one minute.
struct MinuteUse {
  std::string key;
  bool running = true;           // a tree of it was in the latest process table
  Bytes private_bytes;           // commit charge of every member, in the latest process table
  Bytes ram;                     // private working set of every member, likewise
  double cpu_seconds = 0.0;      // CPU time used in the minute
  std::uint64_t disk_bytes = 0;  // I/O transfers in the minute, as Windows counts them (read,
                                 // write and other), like the startup impact
  std::uint64_t net_bytes = 0;   // sent and received (UsageSampler::attribute)

  friend bool operator==(const MinuteUse&, const MinuteUse&) = default;
};

// The network bytes of one process over a time range (events::network_by_process).
struct ProcessTraffic {
  std::uint32_t pid = 0;
  std::optional<WallTime> created;  // whole seconds; nullopt when the PID was not known
  std::string image;                // empty when the PID was not known
  std::uint64_t bytes = 0;
};

// Turns the process tables the telemetry component samples every second into the use of each
// program. Not thread-safe.
class UsageSampler {
 public:
  // How long a process stays known after the last table that had it, so that network minutes,
  // which are stored a little later, still find its tree.
  static constexpr std::chrono::minutes remember{10};

  // Adds the CPU time and I/O each process of `view` used since the previous table that had it,
  // or since its start when it started after the previous table, to the key of its tree; the
  // first table only takes the baseline. A process that starts and exits between two tables is
  // not seen.
  void observe(const telemetry::ProcessView& view);

  // The use since the previous take(), one entry per key, sorted by key: CPU time and I/O summed
  // over the tables observed, memory from the latest one (a key without a tree in it is not
  // running).
  [[nodiscard]] std::vector<MinuteUse> take();

  // Adds `traffic` to `use`: each process's bytes to the key of the tree a table had it in
  // within `remember`, or else to the key of its own image (a new entry that is not running when
  // `use` has none); a process without image is left out. `use` stays sorted by key.
  void attribute(std::span<const ProcessTraffic> traffic, std::vector<MinuteUse>& use) const;

 private:
  // A process instance: PIDs are reused, a PID with its creation time (whole seconds, as the
  // network history keeps it) is not.
  struct Instance {
    std::uint32_t pid = 0;
    std::int64_t created = 0;
    friend auto operator<=>(const Instance&, const Instance&) = default;
  };
  struct Seen {
    std::string key;
    std::chrono::nanoseconds cpu{};
    std::uint64_t io_bytes = 0;
    WallTime last;
  };
  struct Used {
    double cpu_seconds = 0.0;
    std::uint64_t disk_bytes = 0;
  };

  [[nodiscard]] const Seen* find(std::uint32_t pid, std::optional<WallTime> created) const;

  std::map<Instance, Seen> seen_;
  std::optional<WallTime> previous_;
  std::map<std::string, Used, std::less<>> used_;         // since the previous take()
  std::map<std::string, MinuteUse, std::less<>> memory_;  // of the latest table
};

}  // namespace myclean::resident
