// Windows storage reliability counters over WMI (root\Microsoft\Windows\Storage,
// MSFT_StorageReliabilityCounter): the fallback for disks whose health log cannot be read.

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <wil/com.h>
#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: the COM and WMI headers need its types
#include <oleauto.h>
#include <propvarutil.h>
#include <wbemidl.h>
// clang-format on

#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "platform/disk_health/win32_disk_health.hpp"

namespace myclean::platform {

namespace {

constexpr long query_timeout_ms = 60'000;

tl::unexpected<Error> wmi_error(std::string_view what, HRESULT result) {
  const auto code = static_cast<std::uint32_t>(result);
  if (result == WBEM_E_ACCESS_DENIED || result == E_ACCESSDENIED) {
    return fail(Errc::access_denied,
                std::format("{} needs administrator rights (WMI error 0x{:08x})", what, code));
  }
  return fail(Errc::io_error, std::format("{} failed (WMI error 0x{:08x})", what, code));
}

// An integer property; uint64 values arrive as strings and are converted.
std::optional<std::int64_t> integer(IWbemClassObject& row, const wchar_t* name) {
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr))) {
    return std::nullopt;
  }
  const VARTYPE type = value.vt;  // NOLINT(cppcoreguidelines-pro-type-union-access): the tag
  if (type == VT_NULL || type == VT_EMPTY) {
    return std::nullopt;
  }
  LONGLONG converted = 0;
  if (FAILED(::VariantToInt64(value, &converted))) {
    return std::nullopt;
  }
  return converted;
}

}  // namespace

Result<health::ReliabilityCounters> reliability_counters(std::uint32_t disk_number) {
  const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return wmi_error("COM initialization", initialized);
  }
  const auto uninitialize = wil::scope_exit([&] {
    if (SUCCEEDED(initialized)) {
      ::CoUninitialize();
    }
  });

  wil::com_ptr_nothrow<IWbemLocator> locator;
  if (const HRESULT result = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(locator.put()));
      FAILED(result)) {
    return wmi_error("creating the WMI locator", result);
  }
  const wil::unique_bstr name_space{::SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage")};
  wil::com_ptr_nothrow<IWbemServices> services;
  if (const HRESULT result = locator->ConnectServer(name_space.get(), nullptr, nullptr, nullptr, 0,
                                                    nullptr, nullptr, services.put());
      FAILED(result)) {
    return wmi_error("connecting to the storage WMI namespace", result);
  }
  if (const HRESULT result = ::CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT,
                                                 RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                                 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
      FAILED(result)) {
    return wmi_error("setting the WMI proxy security", result);
  }

  const wil::unique_bstr language{::SysAllocString(L"WQL")};
  const wil::unique_bstr query{::SysAllocString(
      std::format(L"SELECT * FROM MSFT_StorageReliabilityCounter WHERE DeviceId = '{}'",
                  disk_number)
          .c_str())};
  wil::com_ptr_nothrow<IEnumWbemClassObject> rows;
  if (const HRESULT result = services->ExecQuery(
          language.get(), query.get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
          nullptr, rows.put());
      FAILED(result)) {
    return wmi_error("querying the reliability counters", result);
  }
  wil::com_ptr_nothrow<IWbemClassObject> row;
  ULONG count = 0;
  if (const HRESULT result = rows->Next(query_timeout_ms, 1, row.put(), &count);
      FAILED(result) || result == WBEM_S_TIMEDOUT) {
    return wmi_error("reading the reliability counters", result);
  }
  if (count == 0) {
    return fail(Errc::not_found,
                std::format("Windows has no reliability counters for disk {}", disk_number));
  }
  return health::ReliabilityCounters{
      .temperature_c = integer(*row, L"Temperature"),
      .wear_percent = integer(*row, L"Wear"),
      .power_on_hours = integer(*row, L"PowerOnHours"),
      .read_errors_uncorrected = integer(*row, L"ReadErrorsUncorrected"),
      .write_errors_uncorrected = integer(*row, L"WriteErrorsUncorrected")};
}

}  // namespace myclean::platform
