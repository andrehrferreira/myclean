#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

// Overlapped message I/O on a message-mode pipe handle opened with FILE_FLAG_OVERLAPPED, shared
// by the pipe server and client. Every wait ends early, cancelling the operation, when `cancel`
// (may be null) is signaled or `timeout_ms` passes (INFINITE for none).
namespace myclean::platform::pipe_io {

// UTF-8 form of a wide string (pipe names, paths) for messages.
[[nodiscard]] std::string utf8(const std::wstring& text);
// A manual-reset event, initially not signaled.
[[nodiscard]] Result<wil::unique_event_nothrow> make_event();

// Errc of a Win32 error from a pipe call; ERROR_BROKEN_PIPE and ERROR_NO_DATA (the other end
// closed) map to invalid_state.
[[nodiscard]] Errc errc_of(DWORD error);
[[nodiscard]] tl::unexpected<Error> failure(std::string_view action, DWORD error);

// Waits for a client on a listening server instance.
[[nodiscard]] Result<void> connect(HANDLE pipe, HANDLE cancel);
// Reads one whole message; fails with invalid_data when it is longer than `max_size`.
[[nodiscard]] Result<std::string> read_message(HANDLE pipe, HANDLE cancel, DWORD timeout_ms,
                                               std::size_t max_size);
[[nodiscard]] Result<void> write_message(HANDLE pipe, std::string_view message, HANDLE cancel,
                                         DWORD timeout_ms);

}  // namespace myclean::platform::pipe_io
