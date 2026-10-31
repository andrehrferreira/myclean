#include "platform/perf/pdh_counters.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <pdh.h>
#include <pdhmsg.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::platform {

namespace {

std::wstring wide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
  return result;
}

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

tl::unexpected<Error> pdh_error(std::string_view action, PDH_STATUS status) {
  const bool missing = status == PDH_CSTATUS_NO_OBJECT || status == PDH_CSTATUS_NO_COUNTER ||
                       status == PDH_CSTATUS_NO_INSTANCE;
  return fail(missing ? Errc::not_found : Errc::io_error,
              std::format("{}: PDH status {:#010x}", action, static_cast<unsigned long>(status)));
}

bool no_value_yet(PDH_STATUS status) {
  return status == PDH_INVALID_DATA || status == PDH_CSTATUS_INVALID_DATA || status == PDH_NO_DATA;
}

class PdhQuery final : public telemetry::ICounterQuery {
 public:
  explicit PdhQuery(PDH_HQUERY query) : query_(query) {}
  PdhQuery(const PdhQuery&) = delete;
  PdhQuery& operator=(const PdhQuery&) = delete;
  PdhQuery(PdhQuery&&) = delete;
  PdhQuery& operator=(PdhQuery&&) = delete;
  ~PdhQuery() override { ::PdhCloseQuery(query_); }

  [[nodiscard]] Result<std::size_t> add(std::string_view path) override {
    PDH_HCOUNTER counter = nullptr;
    const PDH_STATUS status = ::PdhAddEnglishCounterW(query_, wide(path).c_str(), 0, &counter);
    if (status != ERROR_SUCCESS) {
      return pdh_error("cannot add counter", status);
    }
    counters_.push_back(counter);
    return counters_.size() - 1;
  }

  [[nodiscard]] Result<void> collect() override {
    const PDH_STATUS status = ::PdhCollectQueryData(query_);
    if (status != ERROR_SUCCESS && status != PDH_NO_DATA) {
      return pdh_error("cannot collect counters", status);
    }
    return {};
  }

  [[nodiscard]] Result<std::vector<telemetry::CounterValue>> values(
      std::size_t index) const override {
    if (index >= counters_.size()) {
      return fail(Errc::invalid_argument, "no counter " + std::to_string(index));
    }
    // One allocation holds the items followed by the instance names they point to.
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> items;
    DWORD count = 0;
    PDH_STATUS status = PDH_MORE_DATA;
    while (status == PDH_MORE_DATA) {
      auto bytes = static_cast<DWORD>(items.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
      status =
          ::PdhGetFormattedCounterArrayW(counters_[index], PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                         &bytes, &count, items.empty() ? nullptr : items.data());
      if (status == PDH_MORE_DATA) {
        items.resize((bytes / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W)) + 1);
      }
    }
    std::vector<telemetry::CounterValue> values;
    if (no_value_yet(status)) {
      return values;
    }
    if (status != ERROR_SUCCESS) {
      return pdh_error("cannot read counter", status);
    }
    values.reserve(count);
    for (std::size_t item = 0; item < count; ++item) {
      const PDH_FMT_COUNTERVALUE& value = items[item].FmtValue;
      if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
        continue;
      }
      const wchar_t* name = items[item].szName;
      values.push_back(telemetry::CounterValue{
          .instance = name == nullptr ? std::string() : utf8(name),
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): PDH_FMT_DOUBLE selects it
          .value = value.doubleValue});
    }
    return values;
  }

 private:
  PDH_HQUERY query_;
  std::vector<PDH_HCOUNTER> counters_;
};

}  // namespace

Result<std::unique_ptr<telemetry::ICounterQuery>> PdhCounters::open() {
  PDH_HQUERY query = nullptr;
  const PDH_STATUS status = ::PdhOpenQueryW(nullptr, 0, &query);
  if (status != ERROR_SUCCESS) {
    return pdh_error("cannot open a PDH query", status);
  }
  return std::make_unique<PdhQuery>(query);
}

}  // namespace myclean::platform
