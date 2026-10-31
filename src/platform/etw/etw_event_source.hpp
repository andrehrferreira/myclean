#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "core/events/events.hpp"
#include "core/model/result.hpp"

namespace myclean::platform::etw {

// A real-time ETW session, through krabsetw, on Microsoft-Windows-Kernel-Process (process start
// and stop) and Microsoft-Windows-Kernel-Network (TCP and UDP send and receive with the PID).
// Each start is completed before delivery: its NT image path becomes a drive-letter path and its
// command line is read from the live process (the event carries none). The session flushes its
// buffers every second, so a process that exits sooner may be delivered without its command line.
//
// The session is named <prefix><PID of this process>. start() first stops sessions with the same
// prefix whose process no longer runs (left behind by a crash: ETW sessions outlive their
// process); stop() and the destructor always stop the session.
//
// Enabling the two kernel providers needs administrator rights or SYSTEM (the service). Without
// them start() fails with Errc::access_denied, after removing the session it created.
class EtwEventSource final : public events::IEventSource {
 public:
  static constexpr std::wstring_view service_prefix = L"MyClean.Events.";

  explicit EtwEventSource(std::wstring prefix = std::wstring(service_prefix));
  EtwEventSource(const EtwEventSource&) = delete;
  EtwEventSource& operator=(const EtwEventSource&) = delete;
  EtwEventSource(EtwEventSource&&) = delete;
  EtwEventSource& operator=(EtwEventSource&&) = delete;
  ~EtwEventSource() override;

  [[nodiscard]] Result<void> start(events::IEventSink& sink) override;
  void stop() override;

  [[nodiscard]] const std::wstring& session_name() const { return name_; }

 private:
  struct Session;

  std::wstring prefix_;
  std::wstring name_;
  std::unique_ptr<Session> session_;
};

// Stops every ETW session named <prefix><PID> whose process no longer runs, and returns how
// many it stopped. Sessions of running processes and sessions this caller may not stop are left.
int stop_stale_sessions(std::wstring_view prefix);

// Whether an ETW session with this exact name exists.
[[nodiscard]] bool session_exists(std::wstring_view name);

}  // namespace myclean::platform::etw
