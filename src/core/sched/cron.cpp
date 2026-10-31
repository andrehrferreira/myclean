#include "core/sched/cron.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::sched {
namespace {

struct Field {
  std::string_view name;
  int min;
  int max;
};

constexpr std::array<Field, 5> fields{{
    {.name = "minute", .min = 0, .max = 59},
    {.name = "hour", .min = 0, .max = 23},
    {.name = "day-of-month", .min = 1, .max = 31},
    {.name = "month", .min = 1, .max = 12},
    {.name = "day-of-week", .min = 0, .max = 7},
}};

enum FieldIndex : std::size_t { minute_field, hour_field, day_field, month_field, weekday_field };

// Long enough to reach the next 29 February from any date (at most 8 years apart).
constexpr int search_days = 9 * 366;

[[nodiscard]] bool has(std::uint64_t allowed, unsigned value) {
  return ((allowed >> value) & 1U) != 0;
}

// Splits on `separator`, keeping empty parts.
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (std::size_t end = text.find(separator); end != std::string_view::npos;
       end = text.find(separator, start)) {
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  parts.push_back(text.substr(start));
  return parts;
}

// Splits on spaces and tabs, dropping empty parts.
[[nodiscard]] std::vector<std::string_view> split_words(std::string_view text) {
  std::vector<std::string_view> words;
  for (const std::string_view word : split(text, ' ')) {
    for (const std::string_view part : split(word, '\t')) {
      if (!part.empty()) {
        words.push_back(part);
      }
    }
  }
  return words;
}

[[nodiscard]] Result<int> parse_number(std::string_view text, const Field& field) {
  constexpr std::size_t max_digits = 4;
  if (text.empty() || text.size() > max_digits) {
    return fail(Errc::invalid_argument,
                std::format("{} field: '{}' is not a number", field.name, text));
  }
  int value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return fail(Errc::invalid_argument,
                  std::format("{} field: '{}' is not a number", field.name, text));
    }
    value = (value * 10) + (digit - '0');
  }
  return value;
}

[[nodiscard]] Result<int> parse_value(std::string_view text, const Field& field) {
  auto value = parse_number(text, field);
  if (value && (*value < field.min || *value > field.max)) {
    return fail(Errc::invalid_argument, std::format("{} field: {} is out of range {}-{}",
                                                    field.name, *value, field.min, field.max));
  }
  return value;
}

// One item of a list: `*`, `n`, `a-b`, optionally followed by `/step`.
[[nodiscard]] Result<std::uint64_t> parse_item(std::string_view item, const Field& field) {
  const std::size_t slash = item.find('/');
  const std::string_view range = item.substr(0, slash);
  int step = 1;
  if (slash != std::string_view::npos) {
    auto parsed = parse_number(item.substr(slash + 1), field);
    if (!parsed) {
      return tl::unexpected(parsed.error());
    }
    if (*parsed < 1 || *parsed > field.max) {
      return fail(Errc::invalid_argument, std::format("{} field: step {} is out of range 1-{}",
                                                      field.name, *parsed, field.max));
    }
    step = *parsed;
  }

  int low = field.min;
  int high = field.max;
  if (range != "*") {
    const std::size_t dash = range.find('-');
    auto first = parse_value(range.substr(0, dash), field);
    if (!first) {
      return tl::unexpected(first.error());
    }
    low = *first;
    high = slash == std::string_view::npos ? low : field.max;
    if (dash != std::string_view::npos) {
      auto last = parse_value(range.substr(dash + 1), field);
      if (!last) {
        return tl::unexpected(last.error());
      }
      high = *last;
    }
    if (low > high) {
      return fail(Errc::invalid_argument,
                  std::format("{} field: range '{}' runs backwards", field.name, range));
    }
  }

  std::uint64_t allowed = 0;
  for (int value = low; value <= high; value += step) {
    allowed |= std::uint64_t{1} << static_cast<unsigned>(value);
  }
  return allowed;
}

[[nodiscard]] Result<std::uint64_t> parse_field(std::string_view text, const Field& field) {
  std::uint64_t allowed = 0;
  for (const std::string_view item : split(text, ',')) {
    if (item.empty()) {
      return fail(Errc::invalid_argument,
                  std::format("{} field: '{}' has an empty list item", field.name, text));
    }
    auto bits = parse_item(item, field);
    if (!bits) {
      return tl::unexpected(bits.error());
    }
    allowed |= *bits;
  }
  return allowed;
}

}  // namespace

Result<CronExpression> CronExpression::parse(std::string_view text) {
  const std::vector<std::string_view> words = split_words(text);
  if (words.size() != fields.size()) {
    return fail(Errc::invalid_argument,
                std::format("expected 5 fields (minute hour day-of-month month day-of-week), "
                            "got {} in '{}'",
                            words.size(), text));
  }

  CronExpression expression;
  expression.text_ = std::string(text);
  for (std::size_t index = 0; index < fields.size(); ++index) {
    auto allowed = parse_field(words.at(index), fields.at(index));
    if (!allowed) {
      return tl::unexpected(allowed.error());
    }
    expression.allowed_.at(index) = *allowed;
  }
  // Day of week 7 is Sunday, like 0.
  std::uint64_t& weekdays = expression.allowed_.at(weekday_field);
  if (has(weekdays, 7)) {
    weekdays = (weekdays | 1U) & ~(std::uint64_t{1} << 7U);
  }
  expression.either_day_ =
      !words.at(day_field).starts_with('*') && !words.at(weekday_field).starts_with('*');
  return expression;
}

bool CronExpression::matches_day(std::chrono::local_days day) const {
  const std::chrono::year_month_day date{day};
  if (!has(allowed_.at(month_field), static_cast<unsigned>(date.month()))) {
    return false;
  }
  const bool by_date = has(allowed_.at(day_field), static_cast<unsigned>(date.day()));
  const bool by_weekday = has(allowed_.at(weekday_field), std::chrono::weekday{day}.c_encoding());
  return either_day_ ? (by_date || by_weekday) : (by_date && by_weekday);
}

std::optional<WallTime> CronExpression::next_after(WallTime after,
                                                   const std::chrono::time_zone& zone) const {
  using std::chrono::days;
  using std::chrono::hours;
  using std::chrono::local_days;
  using std::chrono::minutes;

  const auto from = std::chrono::floor<minutes>(zone.to_local(after)) + minutes{1};
  const local_days first_day = std::chrono::floor<days>(from);
  for (local_days day = first_day; day < first_day + days{search_days}; day += days{1}) {
    if (!matches_day(day)) {
      continue;
    }
    for (unsigned hour = 0; hour < 24; ++hour) {
      if (!has(allowed_.at(hour_field), hour)) {
        continue;
      }
      for (unsigned minute = 0; minute < 60; ++minute) {
        const auto candidate = day + hours{hour} + minutes{minute};
        if (!has(allowed_.at(minute_field), minute) || candidate < from) {
          continue;
        }
        const WallTime at = zone.to_sys(candidate, std::chrono::choose::earliest);
        if (at > after) {
          return at;
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace myclean::sched
