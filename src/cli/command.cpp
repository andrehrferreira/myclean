#include "cli/command.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "core/mcp/client.hpp"
#include "core/os/clock.hpp"

namespace myclean::cli {

using Json = nlohmann::json;

const std::string* find_option(const CommandLine& line, std::string_view name) {
  const auto found =
      std::ranges::find(line.options, name, &std::pair<std::string, std::string>::first);
  return found == line.options.end() ? nullptr : &found->second;
}

Result<std::optional<std::int64_t>> since_option(const CommandLine& line, const os::IClock& clock) {
  const Result<std::optional<double>> hours = option<double>(line, "hours");
  if (!hours || !*hours) {
    return hours ? Result<std::optional<std::int64_t>>{} : tl::unexpected(hours.error());
  }
  const auto now =
      std::chrono::duration_cast<std::chrono::seconds>(clock.now().time_since_epoch()).count();
  return std::optional<std::int64_t>{now - static_cast<std::int64_t>(**hours * 3600.0)};
}

std::optional<std::int64_t> positive_id(const std::string& text) {
  std::int64_t id = 0;
  const char* const last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(text.data(), last, id);
  if (error != std::errc{} || end != last || id < 1) {
    return std::nullopt;
  }
  return id;
}

std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

int report(const mcp::CallError& error, bool json, std::ostream& out, std::ostream& err) {
  if (json) {
    out << dump(error.reached ? Json{{"error", error.message}}
                              : Json{{"health", "unreachable"}, {"reason", error.message}})
        << '\n';
  } else {
    err << "myclean: " << (error.reached ? error.message : mcp::unavailable_text(error.message))
        << '\n';
  }
  return exit_unavailable;
}

int show(const mcp::Reply& reply, bool json, std::ostream& out, std::ostream& err,
         std::string (*render)(const Json&)) {
  if (!reply) {
    return report(reply.error(), json, out, err);
  }
  out << (json ? dump(*reply) + "\n" : render(*reply));
  return exit_ok;
}

namespace render {

std::optional<double> number(const Json& object, std::string_view key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto found = object.find(key);
  return found != object.end() && found->is_number() ? std::optional(found->get<double>())
                                                     : std::nullopt;
}

std::string text_of(const Json& object, std::string_view key) {
  if (!object.is_object()) {
    return "n/a";
  }
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>() : "n/a";
}

std::string bytes(std::optional<double> value) {
  if (!value) {
    return "n/a";
  }
  constexpr std::array<std::string_view, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
  double size = *value;
  std::size_t unit = 0;
  while (std::abs(size) >= 1024.0 && unit + 1 < units.size()) {
    size /= 1024.0;
    ++unit;
  }
  return unit == 0 ? std::format("{:.0f} B", size) : std::format("{:.1f} {}", size, units.at(unit));
}

std::string fixed(std::optional<double> value, int decimals) {
  return value ? std::format("{:.{}f}", *value, decimals) : "n/a";
}

std::string utc(const Json& object, std::string_view key) {
  const std::optional<double> seconds = number(object, key);
  if (!seconds) {
    return "n/a";
  }
  const std::chrono::sys_seconds time{std::chrono::seconds{static_cast<std::int64_t>(*seconds)}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC", time);
}

Json list_of(const Json& object, std::string_view key) {
  if (!object.is_object()) {
    return Json::array();
  }
  const auto found = object.find(key);
  return found != object.end() && found->is_array() ? *found : Json::array();
}

}  // namespace render
}  // namespace myclean::cli
