#include "core/report/month.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/units.hpp"

namespace myclean::report {

std::optional<std::chrono::year_month> parse_month(std::string_view text) {
  constexpr std::size_t length = 7;
  constexpr std::size_t dash = 4;
  if (text.size() != length || text[dash] != '-') {
    return std::nullopt;
  }
  const auto digits = [](std::string_view part) {
    return std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; });
  };
  const std::string_view year = text.substr(0, dash);
  const std::string_view month = text.substr(dash + 1);
  if (!digits(year) || !digits(month)) {
    return std::nullopt;
  }
  int year_value = 0;
  for (const char c : year) {
    year_value = (year_value * 10) + (c - '0');
  }
  const unsigned month_value =
      (static_cast<unsigned>(month[0] - '0') * 10U) + static_cast<unsigned>(month[1] - '0');
  const std::chrono::year_month parsed{std::chrono::year{year_value},
                                       std::chrono::month{month_value}};
  return parsed.ok() ? std::optional(parsed) : std::nullopt;
}

std::string month_text(std::chrono::year_month month) {
  return std::format("{:04}-{:02}", static_cast<int>(month.year()),
                     static_cast<unsigned>(month.month()));
}

std::chrono::year_month month_of(WallTime time, const std::chrono::time_zone& zone) {
  const auto local = zone.to_local(std::chrono::floor<std::chrono::seconds>(time));
  const std::chrono::year_month_day day{std::chrono::floor<std::chrono::days>(local)};
  return {day.year(), day.month()};
}

bool retry_at_start(std::optional<WallTime> last_run, bool month_has_snapshot, WallTime now,
                    const std::chrono::time_zone& zone) {
  return last_run && !month_has_snapshot && month_of(*last_run, zone) == month_of(now, zone);
}

std::filesystem::path markdown_file(const std::filesystem::path& reports_dir,
                                    std::chrono::year_month month) {
  return reports_dir / month_text(month) / "report.md";
}

std::filesystem::path html_file(const std::filesystem::path& reports_dir,
                                std::chrono::year_month month) {
  return reports_dir / month_text(month) / "report.html";
}

}  // namespace myclean::report
