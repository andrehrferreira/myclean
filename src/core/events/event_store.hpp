#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::events {

// The `events` component's schema:
//  - proc_start: one row per process start. The command line is stored as its hash, not its text
//    (docs/analysis/myclean-planning/07-architecture.md, "Storage schema"): builds start
//    thousands of compilers with long command lines a day, and two years of their text would
//    cost gigabytes; the full text travels with the live event.
//  - net_minute: bytes sent and received per process, protocol and remote endpoint, per minute.
[[nodiscard]] const store::MigrationSet& event_migrations();

// Retention: proc_start like every event table (Store::history_retention), net_minute like
// minute aggregates (Store::minute_retention).
[[nodiscard]] Result<void> add_event_retention(store::Store& store);

// 64-bit FNV-1a of the UTF-8 text, as stored in proc_start.command_line_hash.
[[nodiscard]] std::int64_t command_line_hash(std::string_view command_line);

// Persists events through the store's batched writes (Store::enqueue): every process start goes
// to proc_start; network transfers are summed in memory per minute and written once the minute
// is closed. Thread-safe.
class EventRecorder final : public IEventSink {
 public:
  // `store` must outlive the recorder.
  explicit EventRecorder(store::Store& store);

  void on_start(const ProcessStart& event) override;
  void on_stop(const ProcessStop& event) override;
  void on_network(const NetworkTransfer& transfer) override;

  // Writes the minutes that ended at least Store::close_delay before `now`. A transfer that
  // arrives for a minute already written adds a second row for it; readers sum the rows.
  void close_minutes(WallTime now);
  // Writes every open minute, at shutdown.
  void close_all();

 private:
  struct MinuteKey {
    std::int64_t minute = 0;  // Unix seconds of the minute start
    std::uint32_t pid = 0;
    std::optional<WallTime> created;  // nullopt when the PID was not known
    Protocol protocol = Protocol::tcp;
    IpAddress remote_address;
    std::uint16_t remote_port = 0;

    friend auto operator<=>(const MinuteKey&, const MinuteKey&) = default;
  };
  struct MinuteTotals {
    std::string image;  // empty when the PID was not known
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    WallTime first_seen;
    WallTime last_seen;
  };

  // Writes the minutes before `end` (Unix seconds); called with mutex_ held.
  void write_minutes_before(std::int64_t end);

  store::Store* store_;
  std::mutex mutex_;  // guards open_
  std::map<MinuteKey, MinuteTotals> open_;
};

// Network bytes of one process over a time range.
struct ProcessNetwork {
  std::uint32_t pid = 0;
  std::optional<WallTime> created;  // nullopt when the PID was not known
  std::string image;                // empty when the PID was not known
  std::uint64_t sent = 0;
  std::uint64_t received = 0;
};

// Totals per process of the stored minutes that start in [from, to), most bytes first.
[[nodiscard]] Result<std::vector<ProcessNetwork>> network_by_process(store::Store& store,
                                                                     WallTime from, WallTime to);

// One stored process start (whole seconds, as proc_start keeps them).
struct StoredStart {
  std::uint32_t pid = 0;
  WallTime time;
  std::uint32_t parent_pid = 0;
  std::optional<WallTime> parent_time;
  std::string image;
};

// The process starts stored in [from, to), oldest first.
[[nodiscard]] Result<std::vector<StoredStart>> process_starts(store::Store& store, WallTime from,
                                                              WallTime to);

}  // namespace myclean::events
