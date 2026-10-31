#include "platform/defrag/system_sources.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
#include <winevt.h>
#include <winioctl.h>

#include "core/drives/history.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/defrag/win32_support.hpp"

namespace myclean::platform {

namespace {

using UniqueEvt = wil::unique_any<EVT_HANDLE, decltype(&::EvtClose), ::EvtClose>;

constexpr std::uint64_t filetime_unix_epoch = 116'444'736'000'000'000ULL;
constexpr DWORD batch_size = 64;
constexpr DWORD next_timeout_ms = 5'000;
constexpr BYTE ac_offline = 0;
constexpr BYTE ac_online = 1;
constexpr BYTE no_system_battery = 128;
constexpr int hex_base = 16;

Error evt_error(std::string_view action) {
  return Error{.code = Errc::io_error,
               .message = "reading the optimizer log: " + std::string(action) + ": Win32 error " +
                          std::to_string(::GetLastError())};
}

WallTime from_filetime(std::uint64_t filetime) {
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  return WallTime{std::chrono::duration_cast<WallTime::duration>(
      Ticks{static_cast<std::int64_t>(filetime - filetime_unix_epoch)})};
}

std::string text_of(const EVT_VARIANT& value) {
  if (value.Type != EvtVarTypeString) {
    return {};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): Type checked
  const wchar_t* text = value.StringVal;
  return text == nullptr ? std::string{} : defrag_detail::narrow(text);
}

// Binary data rendered as bytes, or as the hex text the classic event format sometimes yields.
std::vector<std::uint8_t> bytes_of(const EVT_VARIANT& value) {
  if (value.Type == EvtVarTypeBinary) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): Type checked
    const BYTE* bytes = value.BinaryVal;
    if (bytes == nullptr) {
      return {};
    }
    const std::span<const BYTE> data{bytes, value.Count};
    return {data.begin(), data.end()};
  }
  const std::string hex = text_of(value);
  std::vector<std::uint8_t> data;
  for (std::size_t at = 0; at + 1 < hex.size(); at += 2) {
    std::uint8_t byte = 0;
    const std::string_view pair = std::string_view{hex}.substr(at, 2);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the end of `pair`
    static_cast<void>(std::from_chars(pair.data(), pair.data() + pair.size(), byte, hex_base));
    data.push_back(byte);
  }
  return data;
}

// The values of `event` rendered with `context` into `buffer`.
Result<std::span<const EVT_VARIANT>> render(EVT_HANDLE context, EVT_HANDLE event,
                                            std::vector<EVT_VARIANT>& buffer) {
  DWORD used = 0;
  DWORD count = 0;
  while (::EvtRender(context, event, EvtRenderEventValues,
                     static_cast<DWORD>(buffer.size() * sizeof(EVT_VARIANT)), buffer.data(), &used,
                     &count) == FALSE) {
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return tl::unexpected(evt_error("rendering an event"));
    }
    buffer.resize((used / sizeof(EVT_VARIANT)) + 1);
  }
  return std::span<const EVT_VARIANT>{buffer.data(), count};
}

// The system values (time, id) and the event data (operation, volume, binary data) of `event`.
Result<drives::OptimizerEvent> optimizer_event(EVT_HANDLE system_context, EVT_HANDLE user_context,
                                               EVT_HANDLE event,
                                               std::array<std::vector<EVT_VARIANT>, 2>& buffers) {
  const Result<std::span<const EVT_VARIANT>> system = render(system_context, event, buffers[0]);
  if (!system) {
    return tl::unexpected(system.error());
  }
  const Result<std::span<const EVT_VARIANT>> user = render(user_context, event, buffers[1]);
  if (!user) {
    return tl::unexpected(user.error());
  }
  if (system->size() <= EvtSystemTimeCreated || user->size() < 2) {
    return fail(Errc::invalid_data, "an optimizer event rendered too few values");
  }
  drives::OptimizerEvent result{
      .operation = text_of((*user)[0]),
      .volume = text_of((*user)[1]),
      .data = user->size() > 2 ? bytes_of((*user)[2]) : std::vector<std::uint8_t>{}};
  const EVT_VARIANT& time = (*system)[EvtSystemTimeCreated];
  if (time.Type == EvtVarTypeFileTime) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): Type checked
    result.time = from_filetime(time.FileTimeVal);
  }
  const EVT_VARIANT& id = (*system)[EvtSystemEventID];
  if (id.Type == EvtVarTypeUInt16) {
    result.id = id.UInt16Val;  // NOLINT(cppcoreguidelines-pro-type-union-access): Type checked
  }
  return result;
}
}  // namespace

Result<std::vector<drives::OptimizerEvent>> Win32OptimizerLog::events() const {
  const UniqueEvt system_context{::EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem)};
  const UniqueEvt user_context{::EvtCreateRenderContext(0, nullptr, EvtRenderContextUser)};
  if (!system_context || !user_context) {
    return tl::unexpected(evt_error("creating the render contexts"));
  }
  const UniqueEvt query{::EvtQuery(
      nullptr, L"Application",
      L"*[System[Provider[@Name='Microsoft-Windows-Defrag'] and (EventID=258 or EventID=264)]]",
      EvtQueryChannelPath | EvtQueryReverseDirection)};
  if (!query) {
    return tl::unexpected(evt_error("querying the Application log"));
  }
  std::vector<drives::OptimizerEvent> events;
  std::array<std::vector<EVT_VARIANT>, 2> buffers{std::vector<EVT_VARIANT>(32),
                                                  std::vector<EVT_VARIANT>(8)};
  std::array<EVT_HANDLE, batch_size> batch{};
  while (events.size() < max_events) {
    DWORD returned = 0;
    if (::EvtNext(query.get(), batch_size, batch.data(), next_timeout_ms, 0, &returned) == FALSE) {
      if (::GetLastError() == ERROR_NO_MORE_ITEMS) {
        break;
      }
      return tl::unexpected(evt_error("reading the next events"));
    }
    std::vector<UniqueEvt> owned;
    owned.reserve(returned);
    for (const EVT_HANDLE handle : std::span{batch}.first(returned)) {
      owned.emplace_back(handle);
    }
    for (const UniqueEvt& event : owned) {
      Result<drives::OptimizerEvent> rendered =
          optimizer_event(system_context.get(), user_context.get(), event.get(), buffers);
      if (!rendered) {
        return tl::unexpected(rendered.error());
      }
      events.push_back(std::move(*rendered));
    }
  }
  return events;
}

std::optional<bool> Win32TrimSupport::trim_supported(std::uint32_t disk_number) const {
  const std::wstring device = LR"(\\.\PhysicalDrive)" + std::to_wstring(disk_number);
  const wil::unique_hfile disk{::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             nullptr, OPEN_EXISTING, 0, nullptr)};
  if (!disk) {
    return std::nullopt;
  }
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceTrimProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_TRIM_DESCRIPTOR descriptor{};
  DWORD returned = 0;
  if (::DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &descriptor, sizeof(descriptor), &returned, nullptr) == FALSE ||
      returned < sizeof(descriptor)) {
    return std::nullopt;
  }
  return descriptor.TrimEnabled != FALSE;
}

std::optional<bool> Win32TrimSupport::ntfs_trim_enabled() const {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, LR"(SYSTEM\CurrentControlSet\Control\FileSystem)",
                     L"DisableDeleteNotification", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value == 0;
}

Result<bool> Win32PowerSource::on_battery() const {
  SYSTEM_POWER_STATUS status{};
  if (::GetSystemPowerStatus(&status) == FALSE) {
    return fail(Errc::io_error,
                "reading the power status: Win32 error " + std::to_string(::GetLastError()));
  }
  if (status.ACLineStatus == ac_offline) {
    return true;
  }
  if (status.ACLineStatus == ac_online || (status.BatteryFlag & no_system_battery) != 0) {
    return false;
  }
  return fail(Errc::io_error, "Windows does not know whether the computer runs on battery");
}

}  // namespace myclean::platform
