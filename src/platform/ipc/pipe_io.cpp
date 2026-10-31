#include "platform/ipc/pipe_io.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform::pipe_io {

namespace {

constexpr DWORD chunk_size = 64 * 1024;

struct Transfer {
  DWORD bytes = 0;
  bool more_data = false;  // the message continues (ERROR_MORE_DATA)
};

// Waits for the overlapped operation that the call returning `started` began.
Result<Transfer> complete(BOOL started, HANDLE pipe, OVERLAPPED& overlapped, HANDLE cancel,
                          DWORD timeout_ms, std::string_view action) {
  if (started == FALSE) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA) {
      return failure(action, error);
    }
  }
  const std::array<HANDLE, 2> handles{overlapped.hEvent, cancel};
  const DWORD count = cancel == nullptr ? 1 : 2;
  const DWORD waited = ::WaitForMultipleObjects(count, handles.data(), FALSE, timeout_ms);
  if (waited != WAIT_OBJECT_0) {
    const DWORD wait_error = ::GetLastError();
    ::CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);  // the buffer must outlive the I/O
    if (waited == WAIT_TIMEOUT) {
      return fail(Errc::io_error, std::string(action) + ": timed out");
    }
    if (waited == WAIT_OBJECT_0 + 1) {
      return fail(Errc::invalid_state, std::string(action) + ": cancelled");
    }
    return failure(action, wait_error);
  }
  DWORD bytes = 0;
  if (::GetOverlappedResult(pipe, &overlapped, &bytes, FALSE) != FALSE) {
    return Transfer{.bytes = bytes};
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_MORE_DATA) {
    return Transfer{.bytes = bytes, .more_data = true};
  }
  return failure(action, error);
}

}  // namespace

std::string utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const auto length = static_cast<int>(text.size());
  const int size =
      ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, result.data(), size, nullptr, nullptr);
  return result;
}

Result<wil::unique_event_nothrow> make_event() {
  wil::unique_event_nothrow event;
  if (const HRESULT created = event.create(wil::EventOptions::ManualReset); FAILED(created)) {
    return fail(Errc::io_error, "CreateEvent: HRESULT " + std::to_string(created));
  }
  return event;
}

Errc errc_of(DWORD error) {
  switch (error) {
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
      return Errc::invalid_state;
    case ERROR_FILE_NOT_FOUND:
      return Errc::not_found;
    case ERROR_ACCESS_DENIED:
      return Errc::access_denied;
    case ERROR_PIPE_BUSY:
      return Errc::in_use;
    default:
      return Errc::io_error;
  }
}

tl::unexpected<Error> failure(std::string_view action, DWORD error) {
  return fail(errc_of(error), std::string(action) + ": Win32 error " + std::to_string(error));
}

Result<void> connect(HANDLE pipe, HANDLE cancel) {
  Result<wil::unique_event_nothrow> event = make_event();
  if (!event) {
    return tl::unexpected(event.error());
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event->get();
  const BOOL started = ::ConnectNamedPipe(pipe, &overlapped);
  if (started == FALSE && ::GetLastError() == ERROR_PIPE_CONNECTED) {
    return {};  // the client connected between CreateNamedPipe and ConnectNamedPipe
  }
  return complete(started, pipe, overlapped, cancel, INFINITE, "ConnectNamedPipe")
      .transform([](const Transfer&) {});
}

Result<std::string> read_message(HANDLE pipe, HANDLE cancel, DWORD timeout_ms,
                                 std::size_t max_size) {
  Result<wil::unique_event_nothrow> event = make_event();
  if (!event) {
    return tl::unexpected(event.error());
  }
  std::vector<char> chunk(chunk_size);
  std::string message;
  for (;;) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event->get();
    const BOOL started = ::ReadFile(pipe, chunk.data(), chunk_size, nullptr, &overlapped);
    const Result<Transfer> read =
        complete(started, pipe, overlapped, cancel, timeout_ms, "ReadFile(pipe)");
    if (!read) {
      return tl::unexpected(read.error());
    }
    message.append(chunk.data(), read->bytes);
    if (message.size() > max_size) {
      return fail(Errc::invalid_data,
                  "pipe message is longer than " + std::to_string(max_size) + " bytes");
    }
    if (!read->more_data) {
      return message;
    }
  }
}

Result<void> write_message(HANDLE pipe, std::string_view message, HANDLE cancel, DWORD timeout_ms) {
  Result<wil::unique_event_nothrow> event = make_event();
  if (!event) {
    return tl::unexpected(event.error());
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event->get();
  const auto size = static_cast<DWORD>(message.size());
  const BOOL started = ::WriteFile(pipe, message.data(), size, nullptr, &overlapped);
  const Result<Transfer> written =
      complete(started, pipe, overlapped, cancel, timeout_ms, "WriteFile(pipe)");
  if (!written) {
    return tl::unexpected(written.error());
  }
  if (written->bytes != size) {
    return fail(Errc::io_error, "WriteFile(pipe): wrote " + std::to_string(written->bytes) +
                                    " of " + std::to_string(size) + " bytes");
  }
  return {};
}

}  // namespace myclean::platform::pipe_io
