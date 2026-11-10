#include "core/startup/diagnostics.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/impact.hpp"

namespace myclean::startup {
namespace {

constexpr std::size_t npos = std::string_view::npos;

// `text` as a whole number in `base`; nullopt when it is empty, has anything else or overflows.
template <typename T>
std::optional<T> number_of(std::string_view text, int base = 10) {
  T value{};
  const char* const first = std::to_address(text.begin());
  const char* const last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(first, last, value, base);
  if (text.empty() || error != std::errc{} || end != last) {
    return std::nullopt;
  }
  return value;
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The position of the start tag `<name` (followed by a space, '>' or '/') at or after `from`.
std::size_t find_tag(std::string_view xml, std::string_view name, std::size_t from) {
  for (std::size_t at = xml.find('<', from); at != npos; at = xml.find('<', at + 1)) {
    const std::size_t end = at + 1 + name.size();
    if (end < xml.size() && xml.substr(at + 1, name.size()) == name &&
        (is_space(xml[end]) || xml[end] == '>' || xml[end] == '/')) {
      return at;
    }
  }
  return npos;
}

// The value of attribute `name` of start tag `tag` (its text from '<' to '>'), undecoded.
std::optional<std::string_view> attribute(std::string_view tag, std::string_view name) {
  for (std::size_t at = tag.find(name); at != npos; at = tag.find(name, at + 1)) {
    if (at == 0 || !is_space(tag[at - 1])) {
      continue;
    }
    std::size_t next = at + name.size();
    while (next < tag.size() && is_space(tag[next])) {
      ++next;
    }
    if (next >= tag.size() || tag[next] != '=') {
      continue;
    }
    ++next;
    while (next < tag.size() && is_space(tag[next])) {
      ++next;
    }
    if (next >= tag.size() || (tag[next] != '\'' && tag[next] != '"')) {
      continue;
    }
    const std::size_t close = tag.find(tag[next], next + 1);
    if (close == npos) {
      return std::nullopt;
    }
    return tag.substr(next + 1, close - next - 1);
  }
  return std::nullopt;
}

void append_utf8(std::string& out, std::uint32_t code) {
  if (code < 0x80U) {
    out += static_cast<char>(code);
  } else if (code < 0x800U) {
    out += static_cast<char>(0xC0U | (code >> 6U));
    out += static_cast<char>(0x80U | (code & 0x3FU));
  } else if (code < 0x10000U) {
    out += static_cast<char>(0xE0U | (code >> 12U));
    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (code & 0x3FU));
  } else if (code < 0x110000U) {
    out += static_cast<char>(0xF0U | (code >> 18U));
    out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (code & 0x3FU));
  }
}

// `text` with the XML character references (&lt; &gt; &amp; &quot; &apos; &#n; &#xh;) decoded;
// anything else that starts with '&' is kept as it is.
std::string decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t semicolon = text[at] == '&' ? text.find(';', at) : npos;
    if (semicolon == npos) {
      out += text[at++];
      continue;
    }
    const std::string_view entity = text.substr(at + 1, semicolon - at - 1);
    std::optional<std::uint32_t> code;
    if (entity == "lt") {
      code = '<';
    } else if (entity == "gt") {
      code = '>';
    } else if (entity == "amp") {
      code = '&';
    } else if (entity == "quot") {
      code = '"';
    } else if (entity == "apos") {
      code = '\'';
    } else if (entity.size() > 1 && entity.front() == '#') {
      const bool hex = entity[1] == 'x' || entity[1] == 'X';
      code = number_of<std::uint32_t>(entity.substr(hex ? 2 : 1), hex ? 16 : 10);
    }
    if (!code) {
      out += text[at++];
      continue;
    }
    append_utf8(out, *code);
    at = semicolon + 1;
  }
  return out;
}

// The `length` digits of `text` at `at` as a number; nullopt when they are not all digits.
std::optional<int> digits_at(std::string_view text, std::size_t at, std::size_t length) {
  if (at + length > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (const char c : text.substr(at, length)) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = (value * 10) + (c - '0');
  }
  return value;
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// The lower-case file name of `path`, '\' or '/' separated, without surrounding spaces.
std::string file_name(std::string_view path) {
  const std::size_t first = path.find_first_not_of(" \t");
  if (first == npos) {
    return {};
  }
  path = path.substr(first, path.find_last_not_of(" \t") - first + 1);
  const std::size_t slash = path.find_last_of("\\/");
  return lower(slash == npos ? path : path.substr(slash + 1));
}

}  // namespace

std::optional<std::string_view> DiagnosticsEvent::value(std::string_view name) const {
  const auto found = std::ranges::find(data, name, &std::pair<std::string, std::string>::first);
  return found == data.end() ? std::nullopt : std::optional<std::string_view>{found->second};
}

Result<DiagnosticsEvent> parse_event(std::string_view xml) {
  DiagnosticsEvent event;
  const std::size_t id_tag = find_tag(xml, "EventID", 0);
  const std::size_t id_start = id_tag == npos ? npos : xml.find('>', id_tag);
  const std::size_t id_end = id_start == npos ? npos : xml.find('<', id_start);
  if (id_end == npos) {
    return fail(Errc::invalid_data, "a diagnostics event without an EventID");
  }
  const std::optional<std::uint32_t> id =
      number_of<std::uint32_t>(xml.substr(id_start + 1, id_end - id_start - 1));
  if (!id) {
    return fail(Errc::invalid_data, "a diagnostics event whose EventID is not a number");
  }
  event.id = *id;
  if (const std::size_t time_tag = find_tag(xml, "TimeCreated", 0); time_tag != npos) {
    const std::size_t close = xml.find('>', time_tag);
    if (close != npos) {
      if (const std::optional<std::string_view> time =
              attribute(xml.substr(time_tag, close - time_tag), "SystemTime")) {
        event.created = parse_utc(decode(*time));
      }
    }
  }
  for (std::size_t at = find_tag(xml, "Data", 0); at != npos; at = find_tag(xml, "Data", at + 1)) {
    const std::size_t close = xml.find('>', at);
    if (close == npos) {
      break;
    }
    const std::string_view tag = xml.substr(at, close - at);
    const std::optional<std::string_view> name = attribute(tag, "Name");
    std::string text;
    if (tag.back() != '/') {
      const std::size_t end = xml.find("</Data", close);
      if (end == npos) {
        break;
      }
      text = decode(xml.substr(close + 1, end - close - 1));
    }
    if (name) {
      event.data.emplace_back(decode(*name), std::move(text));
    }
  }
  return event;
}

std::optional<WallTime> parse_utc(std::string_view text) {
  constexpr std::size_t seconds_end = 19;  // "YYYY-MM-DDTHH:MM:SS"
  constexpr std::size_t max_fraction = 9;
  if (text.size() <= seconds_end || text.back() != 'Z' || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return std::nullopt;
  }
  const std::optional<int> year = digits_at(text, 0, 4);
  const std::optional<int> month = digits_at(text, 5, 2);
  const std::optional<int> day = digits_at(text, 8, 2);
  const std::optional<int> hour = digits_at(text, 11, 2);
  const std::optional<int> minute = digits_at(text, 14, 2);
  const std::optional<int> second = digits_at(text, 17, 2);
  if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
      *second > 59) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{std::chrono::year{*year},
                                         std::chrono::month{static_cast<unsigned>(*month)},
                                         std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) {
    return std::nullopt;
  }
  std::chrono::nanoseconds fraction{};
  const std::size_t fraction_digits = text.size() - seconds_end - 1;
  if (fraction_digits > 0) {
    if (text[seconds_end] != '.' || fraction_digits - 1 > max_fraction || fraction_digits == 1) {
      return std::nullopt;
    }
    const std::size_t count = fraction_digits - 1;
    const std::optional<int> value = digits_at(text, seconds_end + 1, count);
    if (!value) {
      return std::nullopt;
    }
    std::int64_t nanoseconds = *value;
    for (std::size_t scale = count; scale < max_fraction; ++scale) {
      nanoseconds *= 10;
    }
    fraction = std::chrono::nanoseconds{nanoseconds};
  }
  const auto time = std::chrono::sys_days{date} + std::chrono::hours{*hour} +
                    std::chrono::minutes{*minute} + std::chrono::seconds{*second} + fraction;
  return std::chrono::time_point_cast<WallTime::duration>(time);
}

std::optional<BootDelay> boot_delay(const DiagnosticsEvent& event) {
  constexpr std::uint32_t delay_event = 101;
  if (event.id != delay_event) {
    return std::nullopt;
  }
  std::string image = file_name(event.value("Name").value_or(""));
  if (image.empty()) {
    image = file_name(event.value("Path").value_or(""));
  }
  const std::optional<std::uint32_t> degradation =
      number_of<std::uint32_t>(event.value("DegradationTime").value_or(""));
  if (image.empty() || !degradation) {
    return std::nullopt;
  }
  return BootDelay{
      .image = std::move(image),
      .delay_seconds =
          std::chrono::duration<double>(std::chrono::milliseconds{*degradation}).count()};
}

std::vector<BootDelay> boot_delays(std::span<const std::string> events, WallTime since) {
  std::vector<BootDelay> delays;
  for (const std::string& xml : events) {
    const Result<DiagnosticsEvent> event = parse_event(xml);
    if (!event || !event->created || *event->created < since) {
      continue;
    }
    if (std::optional<BootDelay> delay = boot_delay(*event)) {
      delays.push_back(std::move(*delay));
    }
  }
  return delays;
}

}  // namespace myclean::startup
