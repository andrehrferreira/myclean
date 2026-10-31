#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace myclean::tray_model {

// Shown wherever a value is not available (yet): no sample, or a feature not delivered yet.
inline constexpr std::string_view not_available = "n/a";

// Byte counts in binary units with the labels Windows Explorer uses: "512 B", "1.5 KB",
// "84.0 GB", "1.82 TB". Three significant digits from KB up.
[[nodiscard]] std::string format_bytes(double bytes);
[[nodiscard]] std::string format_bytes(std::optional<double> bytes);
// A transfer rate: format_bytes() + "/s".
[[nodiscard]] std::string format_rate(std::optional<double> bytes_per_second);
// "42 %" (no decimals), "0.5 %" below 1 % (one decimal).
[[nodiscard]] std::string format_percent(std::optional<double> percent);
// Milliseconds: "0.12 ms", "4.5 ms", "120 ms".
[[nodiscard]] std::string format_milliseconds(std::optional<double> milliseconds);
// A plain count with up to one decimal: "3", "0.4", "1520".
[[nodiscard]] std::string format_count(std::optional<double> count);
// A days-until-full projection: "9 days", "1 day", "less than 1 day".
[[nodiscard]] std::string format_days(double days);
// A local time of day, "14:23:05".
[[nodiscard]] std::string format_time_of_day(std::chrono::local_seconds time);

}  // namespace myclean::tray_model
