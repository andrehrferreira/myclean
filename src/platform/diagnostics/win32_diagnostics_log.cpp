#include "platform/diagnostics/win32_diagnostics_log.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
#include <winevt.h>

#include "core/model/result.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {
namespace {

using UniqueEvt = wil::unique_any<EVT_HANDLE, decltype(&::EvtClose), ::EvtClose>;

constexpr DWORD batch_size = 64;
constexpr DWORD next_timeout_ms = 5'000;

tl::unexpected<Error> evt_error(std::string_view action) {
  const DWORD error = ::GetLastError();
  Errc code = Errc::io_error;
  if (error == ERROR_ACCESS_DENIED) {
    code = Errc::access_denied;
  } else if (error == ERROR_EVT_CHANNEL_NOT_FOUND) {
    code = Errc::not_found;
  }
  return fail(code, "reading the Diagnostics-Performance log: " + std::string(action) +
                        ": Win32 error " + std::to_string(error));
}

// `event` rendered as XML into `buffer`.
Result<std::string> render_xml(EVT_HANDLE event, std::vector<wchar_t>& buffer) {
  DWORD used = 0;
  DWORD count = 0;
  while (::EvtRender(nullptr, event, EvtRenderEventXml,
                     static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), buffer.data(), &used,
                     &count) == FALSE) {
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return evt_error("rendering an event");
    }
    buffer.resize((used / sizeof(wchar_t)) + 1);
  }
  // `used` counts the bytes written, the terminating null included.
  const std::size_t length = used / sizeof(wchar_t);
  return settings_detail::narrow(std::wstring_view{buffer.data(), length > 0 ? length - 1 : 0});
}

}  // namespace

Result<std::vector<std::string>> Win32DiagnosticsLog::events() const {
  const UniqueEvt query{::EvtQuery(
      nullptr, L"Microsoft-Windows-Diagnostics-Performance/Operational",
      L"*[System[(EventID=100 or EventID=101)]]", EvtQueryChannelPath | EvtQueryReverseDirection)};
  if (!query) {
    return evt_error("querying the log");
  }
  std::vector<std::string> events;
  std::vector<wchar_t> buffer(4096);
  std::array<EVT_HANDLE, batch_size> batch{};
  while (events.size() < max_events) {
    DWORD returned = 0;
    if (::EvtNext(query.get(), batch_size, batch.data(), next_timeout_ms, 0, &returned) == FALSE) {
      if (::GetLastError() == ERROR_NO_MORE_ITEMS) {
        break;
      }
      return evt_error("reading the next events");
    }
    std::vector<UniqueEvt> owned;
    owned.reserve(returned);
    for (const EVT_HANDLE handle : std::span{batch}.first(returned)) {
      owned.emplace_back(handle);
    }
    for (const UniqueEvt& event : owned) {
      Result<std::string> xml = render_xml(event.get(), buffer);
      if (!xml) {
        return tl::unexpected(xml.error());
      }
      events.push_back(std::move(*xml));
    }
  }
  if (events.size() > max_events) {
    events.resize(max_events);
  }
  return events;
}

}  // namespace myclean::platform
