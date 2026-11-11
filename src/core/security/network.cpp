#include "core/security/network.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events/connections.hpp"
#include "core/events/events.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::security {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t max_pid = 0xFFFFFFFFULL;

std::optional<WallTime> optional_time(store::Statement& statement, int column) {
  if (statement.is_null(column)) {
    return std::nullopt;
  }
  return store::from_unix_seconds(statement.column_int(column));
}

// A process instance as talkers() groups it: PID, creation time and image.
using TalkerKey = std::tuple<std::uint32_t, std::optional<WallTime>, std::string>;
using RemoteKey = std::tuple<events::Protocol, std::string, std::uint16_t>;

struct Building {
  Talker talker;
  std::map<RemoteKey, Remote> remotes;
};

std::optional<WallTime> earliest(std::optional<WallTime> a, std::optional<WallTime> b) {
  if (!a) {
    return b;
  }
  return b ? std::min(*a, *b) : a;
}

std::optional<WallTime> latest(std::optional<WallTime> a, std::optional<WallTime> b) {
  if (!a) {
    return b;
  }
  return b ? std::max(*a, *b) : a;
}

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return text;
}

// The file name of a full path or file name, lower case.
std::string image_name(std::string_view image) {
  const std::size_t separator = image.find_last_of(R"(\/)");
  return lower(
      std::string(separator == std::string_view::npos ? image : image.substr(separator + 1)));
}

std::optional<bool> visible_window(const Talker& talker, const WindowFacts& windows) {
  if (talker.session == 0U) {
    return false;  // a service: no window on the owner's desktop
  }
  const bool named = talker.created && windows.owns(talker.pid, *talker.created);
  if (named || windows.reported) {
    return named;
  }
  return std::nullopt;
}

void add_listener(Talker& talker, const Listener& listener) {
  if (std::ranges::none_of(talker.listening, [&](const Listener& known) {
        return known.protocol == listener.protocol && known.port == listener.port;
      })) {
    talker.listening.push_back(listener);
  }
}

// The talkers of one call of talkers(), process by process.
class TalkerBuilder {
 public:
  explicit TalkerBuilder(std::span<const LiveProcess> live) {
    for (const LiveProcess& process : live) {
      live_by_pid_.emplace(process.pid, &process);
    }
  }

  void add(const Traffic& row) {
    if (local_address(row.remote_address)) {
      return;
    }
    Building& process = entry(row.pid, row.created, row.image);
    Remote& remote = process.remotes[RemoteKey{row.protocol, row.remote_address, row.remote_port}];
    remote.address = row.remote_address;
    remote.port = row.remote_port;
    remote.protocol = row.protocol;
    remote.sent += row.sent;
    remote.received += row.received;
    remote.first_seen = earliest(remote.first_seen, row.first_seen);
    remote.last_seen = latest(remote.last_seen, row.last_seen);
    remote.minutes += row.minutes;
  }

  void add(const events::Connection& connection) {
    const LiveProcess* process = live_of(connection.pid, std::nullopt);
    if (process == nullptr || connection.pid == 0) {
      return;  // a connection of an exited process, or of no process (TIME_WAIT)
    }
    Building& target = instance(*process);
    if (connection.protocol == events::Protocol::udp ||
        connection.state == events::TcpState::listen) {
      add_listener(target.talker,
                   Listener{.protocol = connection.protocol, .port = connection.local_port});
      return;
    }
    if (!connection.remote_address || connection.state == events::TcpState::time_wait) {
      return;
    }
    const std::string address = events::to_string(*connection.remote_address);
    if (local_address(address)) {
      return;
    }
    Remote& remote =
        target.remotes[RemoteKey{events::Protocol::tcp, address, connection.remote_port}];
    remote.address = address;
    remote.port = connection.remote_port;
    remote.protocol = events::Protocol::tcp;
    remote.open_now = true;
  }

  std::vector<Talker> finish(const WindowFacts& windows, std::size_t max_remotes) {
    std::vector<Talker> result;
    for (auto& [key, process] : building_) {
      if (process.remotes.empty() && process.talker.listening.empty()) {
        continue;
      }
      result.push_back(finished(process, windows, max_remotes));
    }
    std::ranges::stable_sort(result, [](const Talker& a, const Talker& b) {
      const std::uint64_t left = a.sent + a.received;
      const std::uint64_t right = b.sent + b.received;
      if (left != right) {
        return left > right;
      }
      return std::tie(a.pid, a.image) < std::tie(b.pid, b.image);
    });
    return result;
  }

 private:
  // The live process with this PID, when it is the instance named (or none is named).
  [[nodiscard]] const LiveProcess* live_of(std::uint32_t pid,
                                           const std::optional<WallTime>& created) const {
    const auto found = live_by_pid_.find(pid);
    if (found == live_by_pid_.end() || (created && found->second->created != *created)) {
      return nullptr;
    }
    return found->second;
  }

  Building& entry(std::uint32_t pid, std::optional<WallTime> created, std::string image) {
    const LiveProcess* process = live_of(pid, created);
    if (process != nullptr && !created) {
      created = process->created;
    }
    if (image.empty() && process != nullptr) {
      image = process->image;
    }
    auto [found, added] = building_.try_emplace(TalkerKey{pid, created, image});
    if (added) {
      found->second.talker =
          Talker{.pid = pid,
                 .created = created,
                 .image = image,
                 .running = process != nullptr,
                 .session = process != nullptr ? process->session : std::nullopt};
    }
    return found->second;
  }

  // The row of a live process: one listed from the history keeps its row (its full image path).
  Building& instance(const LiveProcess& process) {
    for (auto& [key, candidate] : building_) {
      if (std::get<0>(key) == process.pid && std::get<1>(key) == process.created) {
        return candidate;
      }
    }
    return entry(process.pid, process.created, process.image);
  }

  static Talker finished(Building& process, const WindowFacts& windows, std::size_t max_remotes) {
    Talker talker = std::move(process.talker);
    for (auto& [remote_key, remote] : process.remotes) {
      talker.sent += remote.sent;
      talker.received += remote.received;
      talker.first_seen = earliest(talker.first_seen, remote.first_seen);
      talker.last_seen = latest(talker.last_seen, remote.last_seen);
      talker.minutes = std::max(talker.minutes, remote.minutes);
      talker.remotes.push_back(std::move(remote));
    }
    std::ranges::stable_sort(talker.remotes, [](const Remote& a, const Remote& b) {
      return a.sent + a.received > b.sent + b.received;
    });
    if (talker.remotes.size() > max_remotes) {
      talker.remotes.resize(max_remotes);
    }
    std::ranges::sort(talker.listening, [](const Listener& a, const Listener& b) {
      return std::tie(a.protocol, a.port) < std::tie(b.protocol, b.port);
    });
    talker.visible_window = visible_window(talker, windows);
    return talker;
  }

  std::map<std::uint32_t, const LiveProcess*> live_by_pid_;
  std::map<TalkerKey, Building> building_;
};

}  // namespace

Result<std::vector<Traffic>> traffic(store::Store& store, WallTime from, WallTime to) {
  std::vector<Traffic> rows;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(R"sql(
SELECT pid, process_time, IFNULL(image, ''), protocol, remote_address, remote_port, SUM(sent),
       SUM(received), MIN(first_seen), MAX(last_seen), COUNT(DISTINCT time)
FROM net_minute WHERE time >= ?1 AND time < ?2
GROUP BY pid, process_time, image, protocol, remote_address, remote_port)sql");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(from)).bind(2, store::to_unix_seconds(to));
    for (;;) {
      Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      rows.push_back(Traffic{
          .pid = static_cast<std::uint32_t>(statement->column_int(0)),
          .created = optional_time(*statement, 1),
          .image = statement->column_text(2),
          .protocol =
              events::parse_protocol(statement->column_text(3)).value_or(events::Protocol::tcp),
          .remote_address = statement->column_text(4),
          .remote_port = static_cast<std::uint16_t>(statement->column_int(5)),
          .sent = static_cast<std::uint64_t>(statement->column_int(6)),
          .received = static_cast<std::uint64_t>(statement->column_int(7)),
          .first_seen = store::from_unix_seconds(statement->column_int(8)),
          .last_seen = store::from_unix_seconds(statement->column_int(9)),
          .minutes = static_cast<std::uint32_t>(statement->column_int(10))});
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return rows;
}

bool local_address(std::string_view address) {
  return address.empty() || address.starts_with("127.") || address == "::1" ||
         address == "0.0.0.0" || address == "::" || address.starts_with("::ffff:127.");
}

std::vector<Talker> talkers(std::span<const Traffic> history,
                            std::span<const events::Connection> connections,
                            std::span<const LiveProcess> live, const WindowFacts& windows,
                            std::size_t max_remotes) {
  TalkerBuilder builder(live);
  for (const Traffic& row : history) {
    builder.add(row);
  }
  for (const events::Connection& connection : connections) {
    builder.add(connection);
  }
  return builder.finish(windows, max_remotes);
}
ReverseDns::ReverseDns(IResolver& resolver, std::size_t capacity, std::size_t max_queue,
                       std::chrono::seconds ttl)
    : resolver_(&resolver), capacity_(capacity), max_queue_(max_queue), ttl_(ttl) {}

std::optional<std::string> ReverseDns::lookup(const std::string& address, WallTime now) {
  const std::scoped_lock lock(mutex_);
  if (const auto found = cache_.find(address);
      found != cache_.end() && now - found->second.time < ttl_) {
    return found->second.name;
  }
  if (queue_.size() < max_queue_ && queued_.insert(address).second) {
    queue_.push_back(address);
  }
  return std::nullopt;
}

std::size_t ReverseDns::resolve(WallTime now, std::size_t count) {
  std::size_t resolved = 0;
  for (; resolved < count; ++resolved) {
    std::string address;
    {
      const std::scoped_lock lock(mutex_);
      if (queue_.empty()) {
        break;
      }
      address = std::move(queue_.front());
      queue_.pop_front();
    }
    // Outside the lock: a lookup can wait seconds on the resolver.
    std::string name = resolver_->name(address).value_or(std::string());
    const std::scoped_lock lock(mutex_);
    queued_.erase(address);
    if (!cache_.contains(address)) {
      order_.push_back(address);
    }
    cache_.insert_or_assign(address, Entry{.name = std::move(name), .time = now});
    while (cache_.size() > capacity_ && !order_.empty()) {
      cache_.erase(order_.front());
      order_.pop_front();
    }
  }
  return resolved;
}

std::size_t ReverseDns::pending() const {
  const std::scoped_lock lock(mutex_);
  return queue_.size();
}

const store::MigrationSet& security_migrations() {
  static const store::MigrationSet set{
      .component = "security",
      .migrations = {
          {.version = 1, .name = "visible_window_and_file_verdict", .sql = R"sql(
CREATE TABLE visible_window(
  pid INTEGER NOT NULL,
  process_time INTEGER NOT NULL,
  first_seen INTEGER NOT NULL,
  last_seen INTEGER NOT NULL,
  PRIMARY KEY(pid, process_time));
CREATE INDEX visible_window_last ON visible_window(last_seen);
CREATE TABLE file_verdict(
  path TEXT NOT NULL,
  source TEXT NOT NULL,
  size INTEGER NOT NULL,
  modified INTEGER NOT NULL,
  detected INTEGER NOT NULL,
  threat TEXT,
  flagged INTEGER,
  engines INTEGER,
  time INTEGER NOT NULL,
  PRIMARY KEY(path, source));
)sql"},
      }};
  return set;
}

std::string check_windows_record(const Json& record) {
  if (!record.is_object() || record.size() != 1 || !record.contains("pids")) {
    return "the record must be {\"pids\": [integer]}";
  }
  const Json& pids = record.at("pids");
  if (!pids.is_array() || pids.empty() || pids.size() > ipc::max_record_elements) {
    return "pids must hold 1 to 64 process ids";
  }
  for (const Json& pid : pids) {
    const bool fits = pid.is_number_unsigned()
                          ? pid.get<std::uint64_t>() <= max_pid
                          : pid.is_number_integer() && pid.get<std::int64_t>() >= 0 &&
                                pid.get<std::int64_t>() <= static_cast<std::int64_t>(max_pid);
    if (!fits) {
      return "every pid must be an integer from 0 to 4294967295";
    }
  }
  return {};
}

Result<void> WindowLog::load(WallTime since) {
  std::map<std::pair<std::uint32_t, WallTime>, Seen> loaded;
  Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(
        "SELECT pid, process_time, first_seen, last_seen FROM visible_window WHERE last_seen >= "
        "?1");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(since));
    for (;;) {
      Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      const WallTime last = store::from_unix_seconds(statement->column_int(3));
      loaded.insert_or_assign({static_cast<std::uint32_t>(statement->column_int(0)),
                               store::from_unix_seconds(statement->column_int(1))},
                              Seen{.first = store::from_unix_seconds(statement->column_int(2)),
                                   .last = last,
                                   .written = last});
    }
  });
  if (!read) {
    return read;
  }
  const std::scoped_lock lock(mutex_);
  for (auto& [key, seen] : loaded) {
    seen_.try_emplace(key, seen);
    last_report_ = latest(last_report_, seen.last);
  }
  return {};
}

void WindowLog::report(std::span<const std::uint32_t> pids, std::span<const LiveProcess> live,
                       WallTime now) {
  const std::scoped_lock lock(mutex_);
  last_report_ = now;
  for (const std::uint32_t pid : pids) {
    const auto process = std::ranges::find(live, pid, &LiveProcess::pid);
    if (process == live.end()) {
      continue;
    }
    // Whole seconds, as the table and net_minute keep creation times.
    const WallTime created = store::from_unix_seconds(store::to_unix_seconds(process->created));
    const std::pair key{pid, created};
    auto [found, added] = seen_.try_emplace(key, Seen{.first = now, .last = now, .written = now});
    found->second.last = now;
    if (added || now - found->second.written >= write_interval) {
      found->second.written = now;
      store_->enqueue(R"sql(
INSERT INTO visible_window(pid, process_time, first_seen, last_seen) VALUES(?1, ?2, ?3, ?4)
ON CONFLICT(pid, process_time) DO UPDATE SET last_seen = excluded.last_seen)sql",
                      {std::int64_t{pid}, store::to_unix_seconds(created),
                       store::to_unix_seconds(found->second.first), store::to_unix_seconds(now)});
    }
  }
}

WindowFacts WindowLog::facts(WallTime from) const {
  WindowFacts facts;
  const std::scoped_lock lock(mutex_);
  facts.reported = last_report_ && *last_report_ >= from;
  for (const auto& [key, seen] : seen_) {
    if (seen.last >= from) {
      facts.owners.push_back(key);
    }
  }
  return facts;
}

}  // namespace myclean::security
