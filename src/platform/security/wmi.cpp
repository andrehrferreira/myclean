#include "platform/security/wmi.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: the COM and WMI headers need its types
#include <oleauto.h>
#include <wbemidl.h>
// clang-format on

#include "core/model/result.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform::security_detail {
namespace {

constexpr long row_timeout_ms = 60'000;
constexpr ULONG batch = 64;

tl::unexpected<Error> wmi_error(std::string_view what, HRESULT result) {
  const auto code = static_cast<std::uint32_t>(result);
  if (result == WBEM_E_ACCESS_DENIED || result == E_ACCESSDENIED) {
    return fail(Errc::access_denied,
                std::format("{} needs administrator rights (WMI error 0x{:08x})", what, code));
  }
  if (result == WBEM_E_INVALID_NAMESPACE || result == WBEM_E_INVALID_CLASS) {
    return fail(Errc::not_found, std::format("{}: not available (WMI error 0x{:08x})", what, code));
  }
  return fail(Errc::io_error, std::format("{} failed (WMI error 0x{:08x})", what, code));
}

}  // namespace

ComScope::ComScope() : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

ComScope::~ComScope() {
  if (SUCCEEDED(result_)) {
    ::CoUninitialize();
  }
}

Result<void> ComScope::status() const {
  if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
    return wmi_error("COM initialization", result_);
  }
  return {};
}

Result<std::vector<WmiRow>> wmi_query(std::wstring_view name_space, std::wstring_view query) {
  const std::string what = settings_detail::narrow(name_space);
  wil::com_ptr_nothrow<IWbemLocator> locator;
  if (const HRESULT result = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(locator.put()));
      FAILED(result)) {
    return wmi_error("creating the WMI locator", result);
  }
  const wil::unique_bstr space{::SysAllocString(std::wstring(name_space).c_str())};
  wil::com_ptr_nothrow<IWbemServices> services;
  if (const HRESULT result = locator->ConnectServer(space.get(), nullptr, nullptr, nullptr, 0,
                                                    nullptr, nullptr, services.put());
      FAILED(result)) {
    return wmi_error("connecting to " + what, result);
  }
  if (const HRESULT result = ::CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT,
                                                 RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                                 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
      FAILED(result)) {
    return wmi_error("setting the WMI proxy security", result);
  }
  const wil::unique_bstr language{::SysAllocString(L"WQL")};
  const wil::unique_bstr text{::SysAllocString(std::wstring(query).c_str())};
  wil::com_ptr_nothrow<IEnumWbemClassObject> rows;
  if (const HRESULT result = services->ExecQuery(
          language.get(), text.get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
          nullptr, rows.put());
      FAILED(result)) {
    return wmi_error("querying " + what, result);
  }
  std::vector<WmiRow> result;
  for (;;) {
    std::vector<IWbemClassObject*> raw(batch, nullptr);
    ULONG count = 0;
    const HRESULT next = rows->Next(row_timeout_ms, batch, raw.data(), &count);
    for (ULONG index = 0; index < count; ++index) {
      WmiRow row;
      row.attach(raw[index]);
      result.push_back(std::move(row));
    }
    if (FAILED(next) || next == WBEM_S_TIMEDOUT) {
      return wmi_error("reading " + what, next);
    }
    if (next == WBEM_S_FALSE || count == 0) {
      return result;
    }
  }
}

std::string wmi_text(IWbemClassObject& row, const wchar_t* name) {
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr))) {
    return {};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): the VARIANT tag and its BSTR
  if (value.vt != VT_BSTR || value.bstrVal == nullptr) {
    return {};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): the tag says BSTR
  return settings_detail::narrow({value.bstrVal, ::SysStringLen(value.bstrVal)});
}

std::vector<std::string> wmi_texts(IWbemClassObject& row, const wchar_t* name) {
  std::vector<std::string> texts;
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr))) {
    return texts;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): the VARIANT tag and its array
  if (value.vt != (VT_ARRAY | VT_BSTR) || value.parray == nullptr) {
    return texts;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): the tag says BSTR array
  SAFEARRAY* array = value.parray;
  LONG lower = 0;
  LONG upper = -1;
  if (FAILED(::SafeArrayGetLBound(array, 1, &lower)) ||
      FAILED(::SafeArrayGetUBound(array, 1, &upper))) {
    return texts;
  }
  for (LONG index = lower; index <= upper; ++index) {
    BSTR item = nullptr;
    if (SUCCEEDED(::SafeArrayGetElement(array, &index, static_cast<void*>(&item))) &&
        item != nullptr) {
      texts.push_back(settings_detail::narrow({item, ::SysStringLen(item)}));
      ::SysFreeString(item);
    }
  }
  return texts;
}

}  // namespace myclean::platform::security_detail
