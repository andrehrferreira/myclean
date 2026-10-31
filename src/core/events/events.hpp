#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::events {

// A process instance: PIDs are reused, a PID with its creation time is not.
struct ProcessKey {
  std::uint32_t pid = 0;
  WallTime created;

  friend auto operator<=>(const ProcessKey&, const ProcessKey&) = default;
};

struct ProcessStart {
  ProcessKey process;
  std::uint32_t parent_pid = 0;  // the creator's PID as the OS reported it
  // The parent instance, set by EventRouter only when parent_pid belongs to a live process created
  // before this one, so a reused PID is never taken for the parent.
  std::optional<ProcessKey> parent;
  std::uint32_t session = 0;
  // Full path, UTF-8. File name only for processes that were already running when the event
  // source started (seeded from the process table).
  std::string image;
  // Read from the process when its start is delivered; nullopt when it already exited or the
  // process denies access.
  std::optional<std::string> command_line;
  std::optional<std::string> signer;  // set by EventRouter through ISignerLookup
};

struct ProcessStop {
  ProcessKey process;
  WallTime exited;
  std::uint32_t exit_code = 0;
  std::string image;  // file name, as the stop event reports it
};

enum class Protocol { tcp, udp };
[[nodiscard]] std::string_view to_string(Protocol protocol);
[[nodiscard]] std::optional<Protocol> parse_protocol(std::string_view text);

enum class Direction { send, receive };

struct IpAddress {
  std::array<std::uint8_t, 16> bytes{};  // network order; IPv4 uses the first 4 bytes
  bool v6 = false;

  friend auto operator<=>(const IpAddress&, const IpAddress&) = default;
};
// "192.0.2.1"; IPv6 in the RFC 5952 form ("2001:db8::1").
[[nodiscard]] std::string to_string(const IpAddress& address);

// One send or receive of one process.
struct NetworkTransfer {
  WallTime time;
  std::uint32_t pid = 0;
  Protocol protocol = Protocol::tcp;
  Direction direction = Direction::send;
  IpAddress remote_address;
  std::uint16_t remote_port = 0;
  std::uint64_t bytes = 0;
  // The live process with this PID, set by EventRouter; null when the PID is not known.
  std::shared_ptr<const ProcessStart> process;
};

// Receives events on the event source's thread. Implementations must return quickly (copy and
// hand off): a slow sink delays every later event, and ETW drops events when its buffers fill.
class IEventSink {
 public:
  virtual ~IEventSink() = default;
  IEventSink(const IEventSink&) = delete;
  IEventSink& operator=(const IEventSink&) = delete;
  IEventSink(IEventSink&&) = delete;
  IEventSink& operator=(IEventSink&&) = delete;

  virtual void on_start(const ProcessStart& event) = 0;
  virtual void on_stop(const ProcessStop& event) = 0;
  virtual void on_network(const NetworkTransfer& transfer) = 0;

 protected:
  IEventSink() = default;
};

// Real-time process and network events. Implemented by src/platform/etw.
class IEventSource {
 public:
  virtual ~IEventSource() = default;
  IEventSource(const IEventSource&) = delete;
  IEventSource& operator=(const IEventSource&) = delete;
  IEventSource(IEventSource&&) = delete;
  IEventSource& operator=(IEventSource&&) = delete;

  // Delivers events to `sink` on the source's own thread until stop(). Fails with the reason when
  // real-time events are not available (for example, not running elevated).
  [[nodiscard]] virtual Result<void> start(IEventSink& sink) = 0;
  // Stops delivering; returns after the last event was delivered. Idempotent.
  virtual void stop() = 0;

 protected:
  IEventSource() = default;
};

// The signer of an image, implemented by the trust module. Called on the event thread for every
// process start, so it must answer quickly (from a cache).
class ISignerLookup {
 public:
  virtual ~ISignerLookup() = default;
  ISignerLookup(const ISignerLookup&) = delete;
  ISignerLookup& operator=(const ISignerLookup&) = delete;
  ISignerLookup(ISignerLookup&&) = delete;
  ISignerLookup& operator=(ISignerLookup&&) = delete;

  // nullopt for an unsigned image or one whose signature could not be verified.
  [[nodiscard]] virtual std::optional<std::string> signer(std::string_view image) = 0;

 protected:
  ISignerLookup() = default;
};

}  // namespace myclean::events
