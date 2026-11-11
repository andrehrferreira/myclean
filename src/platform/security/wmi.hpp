#pragma once

// A small WMI reader for the security adapters: one query, its rows, and their properties.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <wil/com.h>

// clang-format off
#include <windows.h>  // first: the COM and WMI headers need its types
#include <wbemidl.h>
// clang-format on

#include "core/model/result.hpp"

namespace myclean::platform::security_detail {

using WmiRow = wil::com_ptr_nothrow<IWbemClassObject>;

// COM for the calling thread for the lifetime of the scope, in whatever apartment it already has.
class ComScope {
 public:
  ComScope();
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  ComScope(ComScope&&) = delete;
  ComScope& operator=(ComScope&&) = delete;
  ~ComScope();

  [[nodiscard]] Result<void> status() const;

 private:
  HRESULT result_;
};

// Every row of `query` (WQL) in `name_space`; the caller holds a ComScope. Waits at most 60 s.
[[nodiscard]] Result<std::vector<WmiRow>> wmi_query(std::wstring_view name_space,
                                                    std::wstring_view query);

// A string property as UTF-8; empty when null or missing.
[[nodiscard]] std::string wmi_text(IWbemClassObject& row, const wchar_t* name);
// A string-array property as UTF-8; empty when null or missing.
[[nodiscard]] std::vector<std::string> wmi_texts(IWbemClassObject& row, const wchar_t* name);

}  // namespace myclean::platform::security_detail
