#include "platform/etw/etw_event_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <wil/resource.h>

#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "platform/etw/etw_records.hpp"
#include "platform/etw/krabs_user.hpp"
#include "platform/etw/process_details.hpp"

namespace myclean::platform::etw {

namespace {

// QueryAllTraces accepts at most 64 sessions, the default ETW limit.
constexpr ULONG max_sessions = 64;
constexpr std::size_t max_name = 1024;

// EVENT_TRACE_PROPERTIES followed by room for the session and log file names.
struct SessionProperties {
  EVENT_TRACE_PROPERTIES properties{};
  std::array<wchar_t, max_name> session_name{};
  std::array<wchar_t, max_name> log_file_name{};
};

void prepare(SessionProperties& entry) {
  entry = {};
  entry.properties.Wnode.BufferSize = sizeof(SessionProperties);
  entry.properties.LoggerNameOffset = offsetof(SessionProperties, session_name);
  entry.properties.LogFileNameOffset = offsetof(SessionProperties, log_file_name);
}

std::vector<std::wstring> session_names() {
  std::vector<SessionProperties> entries(max_sessions);
  std::vector<EVENT_TRACE_PROPERTIES*> pointers;
  for (SessionProperties& entry : entries) {
    prepare(entry);
    pointers.push_back(&entry.properties);
  }
  ULONG count = 0;
  const ULONG status = ::QueryAllTracesW(pointers.data(), max_sessions, &count);
  std::vector<std::wstring> names;
  if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) {
    return names;
  }
  for (ULONG index = 0; index < count && index < max_sessions; ++index) {
    names.emplace_back(entries[index].session_name.data());
  }
  return names;
}

bool process_running(DWORD pid) {
  const wil::unique_handle process{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!process) {
    return ::GetLastError() != ERROR_INVALID_PARAMETER;  // access denied: it exists
  }
  DWORD exit_code = 0;
  return ::GetExitCodeProcess(process.get(), &exit_code) != FALSE && exit_code == STILL_ACTIVE;
}

ULONG stop_session(const std::wstring& name) {
  SessionProperties entry{};
  prepare(entry);
  return ::ControlTraceW(0, name.c_str(), &entry.properties, EVENT_TRACE_CONTROL_STOP);
}

}  // namespace

struct EtwEventSource::Session {
  explicit Session(std::wstring name) : trace_name(std::move(name)), trace(trace_name) {}

  // Stops the session; ProcessTrace on the consumer thread then returns.
  void stop() noexcept {
    try {
      trace.stop();
    } catch (const std::exception&) {
      // krabs failed to stop or close: stop by name, so no session outlives this object.
      stop_session(trace_name);
    }
    if (consumer.joinable()) {
      consumer.join();
    }
  }

  std::wstring trace_name;
  krabs::trace<krabs::details::ut> trace;  // krabs::user_trace, without krabs/client.hpp
  krabs::provider<> processes{kernel_process_provider};
  krabs::provider<> network{kernel_network_provider};
  DevicePaths paths;  // used on the consumer thread only
  std::thread consumer;
};

EtwEventSource::EtwEventSource(std::wstring prefix)
    : prefix_(std::move(prefix)), name_(prefix_ + std::to_wstring(::GetCurrentProcessId())) {}

EtwEventSource::~EtwEventSource() {
  stop();
}

Result<void> EtwEventSource::start(events::IEventSink& sink) {
  if (session_) {
    return fail(Errc::invalid_state, "the ETW session is already running");
  }
  stop_stale_sessions(prefix_);

  auto session = std::make_unique<Session>(name_);
  EVENT_TRACE_PROPERTIES properties{};
  properties.BufferSize = 64;  // KB
  properties.MinimumBuffers = 4;
  properties.MaximumBuffers = 64;
  properties.FlushTimer = 1;  // seconds: the longest delay before an event is delivered
  properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
  session->trace.set_trace_properties(&properties);

  Session* const raw = session.get();
  session->processes.any(process_keyword);
  session->processes.level(TRACE_LEVEL_INFORMATION);
  session->processes.add_on_event_callback(
      [raw, &sink](const EVENT_RECORD& record, const krabs::trace_context& context) noexcept {
        try {
          if (std::optional<events::ProcessStart> start =
                  process_start_of(record, context.schema_locator)) {
            start->image = raw->paths.to_dos(start->image);
            start->command_line = command_line_of(start->process);
            sink.on_start(*start);
          } else if (const std::optional<events::ProcessStop> stop =
                         process_stop_of(record, context.schema_locator)) {
            sink.on_stop(*stop);
          }
          // An exception must not unwind into ProcessTrace: an event that cannot be delivered
          // (out of memory) is dropped, as ETW itself drops events under load.
          // NOLINTNEXTLINE(bugprone-empty-catch): see above
        } catch (const std::exception&) {
        }
      });
  session->network.any(network_keywords);
  session->network.level(TRACE_LEVEL_INFORMATION);
  session->network.add_on_event_callback(
      [&sink](const EVENT_RECORD& record, const krabs::trace_context& context) noexcept {
        try {
          if (const std::optional<events::NetworkTransfer> transfer =
                  network_transfer_of(record, context.schema_locator)) {
            sink.on_network(*transfer);
          }
          // NOLINTNEXTLINE(bugprone-empty-catch): dropped, as above
        } catch (const std::exception&) {
        }
      });
  session->trace.enable(session->processes);
  session->trace.enable(session->network);

  try {
    (void)session->trace.open();
  } catch (const krabs::need_to_be_admin_failure&) {
    session->stop();
    return fail(Errc::access_denied,
                "enabling Microsoft-Windows-Kernel-Process and Microsoft-Windows-Kernel-Network "
                "needs administrator rights or SYSTEM");
  } catch (const std::exception& error) {
    session->stop();
    return fail(Errc::io_error, std::format("the ETW session could not start: {}", error.what()));
  }
  session->consumer = std::thread([raw] {
    try {
      raw->trace.process();
      // NOLINTNEXTLINE(bugprone-empty-catch): delivery ended; stop() still stops the session
    } catch (const std::exception&) {
    }
  });
  session_ = std::move(session);
  return {};
}

void EtwEventSource::stop() {
  if (session_) {
    session_->stop();
    session_.reset();
  }
}

int stop_stale_sessions(std::wstring_view prefix) {
  int stopped = 0;
  for (const std::wstring& name : session_names()) {
    if (!name.starts_with(prefix)) {
      continue;
    }
    const std::wstring_view suffix = std::wstring_view{name}.substr(prefix.size());
    DWORD pid = 0;
    try {
      std::size_t used = 0;
      pid = static_cast<DWORD>(std::stoul(std::wstring(suffix), &used));
      if (used != suffix.size()) {
        continue;
      }
    } catch (const std::exception&) {
      continue;
    }
    if (!process_running(pid) && stop_session(name) == ERROR_SUCCESS) {
      ++stopped;
    }
  }
  return stopped;
}

bool session_exists(std::wstring_view name) {
  const std::vector<std::wstring> names = session_names();
  return std::ranges::find(names, name) != names.end();
}

}  // namespace myclean::platform::etw
