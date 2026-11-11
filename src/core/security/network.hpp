#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events/connections.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

// Background network talkers over time (security spec, "Background Network Talkers Over Time"):
// the per-minute traffic the events component records (net_minute) over a time window, the
// current TCP/UDP tables, whether each process owned a visible window, and reverse-DNS names
// when the owner turned them on.
namespace myclean::security {

// The traffic of one process with one remote endpoint over a window, summed from net_minute.
struct Traffic {
  std::uint32_t pid = 0;
  std::optional<WallTime> created;  // nullopt when the PID was not known
  std::string image;                // full path or file name; empty when the PID was not known
  events::Protocol protocol = events::Protocol::tcp;
  std::string remote_address;
  std::uint16_t remote_port = 0;
  std::uint64_t sent = 0;
  std::uint64_t received = 0;
  WallTime first_seen;
  WallTime last_seen;
  std::uint32_t minutes = 0;  // minutes with traffic
};

// The net_minute rows of minutes starting in [from, to), summed per process and endpoint.
[[nodiscard]] Result<std::vector<Traffic>> traffic(store::Store& store, WallTime from, WallTime to);

// A process of the current process table.
struct LiveProcess {
  std::uint32_t pid = 0;
  WallTime created;
  std::string image;  // file name
  std::optional<std::uint32_t> session;
};

struct Remote {
  std::string address;
  std::uint16_t port = 0;
  events::Protocol protocol = events::Protocol::tcp;
  std::uint64_t sent = 0;
  std::uint64_t received = 0;
  std::optional<WallTime> first_seen;  // nullopt: only an open connection, no traffic recorded
  std::optional<WallTime> last_seen;
  std::uint32_t minutes = 0;
  bool open_now = false;  // a TCP connection to it is in the current table
};

struct Listener {
  events::Protocol protocol = events::Protocol::tcp;
  std::uint16_t port = 0;
};

struct Talker {
  std::uint32_t pid = 0;
  std::optional<WallTime> created;
  std::string image;
  bool running = false;
  std::optional<std::uint32_t> session;
  std::optional<bool> visible_window;  // nullopt: unknown
  std::uint64_t sent = 0;
  std::uint64_t received = 0;
  std::optional<WallTime> first_seen;
  std::optional<WallTime> last_seen;
  std::uint32_t minutes = 0;    // the most minutes of traffic with any one endpoint
  std::vector<Remote> remotes;  // most bytes first, then open connections
  std::vector<Listener> listening;
};

// Which processes owned a visible window, as the tray reported them.
struct WindowFacts {
  std::vector<std::pair<std::uint32_t, WallTime>> owners;  // (pid, created)
  bool reported = false;  // the tray reported at least once in the window

  [[nodiscard]] bool owns(std::uint32_t pid, WallTime created) const {
    return std::ranges::find(owners, std::pair{pid, created}) != owners.end();
  }
};

// Every process that used the network: the history of `history` and the current `connections`
// (TCP connections to a remote address, TCP listeners and UDP endpoints) of `live` processes.
// Loopback and unspecified remote addresses are left out. A process in session 0 has no visible
// window; another one has one when `windows` names it, none when the tray reported and did not
// name it, and unknown when the tray never reported. Most bytes first; processes with only open
// connections or listeners last, by PID. At most `max_remotes` endpoints per process.
[[nodiscard]] std::vector<Talker> talkers(std::span<const Traffic> history,
                                          std::span<const events::Connection> connections,
                                          std::span<const LiveProcess> live,
                                          const WindowFacts& windows, std::size_t max_remotes = 20);

// Whether `address` (text form) is loopback or unspecified.
[[nodiscard]] bool local_address(std::string_view address);

// Reverse DNS of one address. Implemented by src/platform/security; tests use fakes. Blocking.
class IResolver {
 public:
  virtual ~IResolver() = default;
  IResolver(const IResolver&) = delete;
  IResolver& operator=(const IResolver&) = delete;
  IResolver(IResolver&&) = delete;
  IResolver& operator=(IResolver&&) = delete;

  // The name of `address`; nullopt when it has no reverse-DNS record or the lookup failed.
  [[nodiscard]] virtual std::optional<std::string> name(std::string_view address) = 0;

 protected:
  IResolver() = default;
};

// Reverse-DNS names, cached and resolved off the caller's thread: lookup() answers from the cache
// and queues an address it does not know; resolve() (a worker's loop) resolves queued addresses.
// Bounded: at most `capacity` names are kept (the oldest are dropped) and `max_queue` addresses
// wait; a name is kept for `ttl`. Thread-safe.
class ReverseDns {
 public:
  static constexpr std::size_t default_capacity = 4096;
  static constexpr std::size_t default_queue = 256;
  static constexpr std::chrono::hours default_ttl{6};

  explicit ReverseDns(IResolver& resolver, std::size_t capacity = default_capacity,
                      std::size_t max_queue = default_queue,
                      std::chrono::seconds ttl = default_ttl);

  // The cached result: a name or "" (no record); nullopt when not resolved yet (then queued).
  [[nodiscard]] std::optional<std::string> lookup(const std::string& address, WallTime now);
  // Resolves up to `count` queued addresses; returns how many it resolved.
  std::size_t resolve(WallTime now, std::size_t count);
  [[nodiscard]] std::size_t pending() const;

 private:
  struct Entry {
    std::string name;
    WallTime time;
  };

  IResolver* resolver_;
  std::size_t capacity_;
  std::size_t max_queue_;
  std::chrono::seconds ttl_;
  mutable std::mutex mutex_;  // guards the members below
  std::map<std::string, Entry> cache_;
  std::deque<std::string> order_;  // cache_ keys, oldest first
  std::deque<std::string> queue_;
  std::set<std::string> queued_;
};

// Table visible_window: the processes the tray reported with a visible window, one row per
// process instance with the first and last report.
[[nodiscard]] const store::MigrationSet& security_migrations();

// What the tray reports in security.windows: {"pids": [integer]} (1..64 process ids). The reason
// `record` does not fit, or an empty string.
[[nodiscard]] std::string check_windows_record(const nlohmann::json& record);

// Keeps the visible-window reports: in memory, and in visible_window through the store's batched
// writes (a process at most once per `write_interval`). Thread-safe.
class WindowLog {
 public:
  static constexpr std::chrono::minutes write_interval{10};

  // `store` (opened with security_migrations()) must outlive the log.
  explicit WindowLog(store::Store& store) : store_(&store) {}

  // Loads the reports of processes seen since `since`.
  [[nodiscard]] Result<void> load(WallTime since);
  // The processes of `pids` that are in `live` own a visible window at `now`.
  void report(std::span<const std::uint32_t> pids, std::span<const LiveProcess> live, WallTime now);
  // The facts over [from, ...): owners seen since `from`, and whether any report came since.
  [[nodiscard]] WindowFacts facts(WallTime from) const;

 private:
  struct Seen {
    WallTime first;
    WallTime last;
    WallTime written;
  };

  store::Store* store_;
  mutable std::mutex mutex_;  // guards the members below
  std::map<std::pair<std::uint32_t, WallTime>, Seen> seen_;
  std::optional<WallTime> last_report_;
};

}  // namespace myclean::security
