#include "core/input/incident_record.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::input {

namespace {

using Json = nlohmann::json;
using std::chrono::milliseconds;

constexpr std::int64_t max_duration_ms = 3'600'000;
constexpr std::int64_t max_id = 0xFFFF'FFFF;
constexpr std::int64_t max_time = 253'402'300'799;  // 9999-12-31T23:59:59Z

constexpr std::array<std::string_view, 15> record_members = {
    "chord",        "time",       "latency_ms", "capped",      "owner_unanswered_ms",
    "input_lag_ms", "foreground", "owner",      "holder",      "viewer",
    "formats",      "hung",       "suspects",   "cpu_percent", "memory_load_percent"};

// `text` fitted to the schema: every control character becomes '?', and text longer than
// record_string_limit bytes is cut at a character boundary and ends in "...".
std::string bounded(std::string_view text) {
  constexpr unsigned char first_printable = 0x20;
  constexpr unsigned char del = 0x7F;
  constexpr std::string_view ellipsis = "...";
  std::string result(text);
  std::ranges::replace_if(
      result,
      [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < first_printable || byte == del;
      },
      '?');
  if (result.size() > record_string_limit) {
    std::size_t cut = record_string_limit - ellipsis.size();
    constexpr unsigned char continuation_mask = 0xC0;
    constexpr unsigned char continuation = 0x80;
    while (cut > 0 &&
           (static_cast<unsigned char>(result[cut]) & continuation_mask) == continuation) {
      --cut;  // never split a UTF-8 sequence
    }
    result.resize(cut);
    result += ellipsis;
  }
  return result;
}

Json process_record(const ProcessRef& process) {
  return Json{{"pid", process.pid}, {"image", bounded(process.image)}};
}

Json process_record(const std::optional<ProcessRef>& process) {
  return process ? process_record(*process) : Json(nullptr);
}

// The entries of `list` made by `entry`; when there are more than record_list_limit, the first
// record_list_limit - 1 and then `more(count of the rest)` as the last entry.
template <typename T, typename Entry, typename More>
Json bounded_list(const std::vector<T>& list, Entry entry, More more) {
  Json result = Json::array();
  const bool cut = list.size() > record_list_limit;
  const std::size_t kept = cut ? record_list_limit - 1 : list.size();
  for (std::size_t index = 0; index < kept; ++index) {
    result.push_back(entry(list[index]));
  }
  if (cut) {
    result.push_back(more(list.size() - kept));
  }
  return result;
}

std::string more_text(std::size_t count) {
  return "(" + std::to_string(count) + " more)";
}

std::int64_t bounded_ms(milliseconds duration) {
  return std::clamp<std::int64_t>(duration.count(), 0, max_duration_ms);
}

// Why a record does not fit; thrown by the readers below and caught in parse_record().
struct Problem {
  std::string message;
};

std::int64_t integer(const Json& value, std::string_view name, std::int64_t high) {
  // An unsigned value above INT64_MAX is out of range; get<std::int64_t>() would wrap it.
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() > static_cast<std::uint64_t>(high))) {
    throw Problem{std::string(name) + " must be an integer from 0 to " + std::to_string(high)};
  }
  const auto number = value.get<std::int64_t>();
  if (number < 0 || number > high) {
    throw Problem{std::string(name) + " must be an integer from 0 to " + std::to_string(high)};
  }
  return number;
}

std::string text(const Json& value, std::string_view name) {
  if (!value.is_string()) {
    throw Problem{std::string(name) + " must be a string"};
  }
  const auto& string = value.get_ref<const std::string&>();
  constexpr unsigned char first_printable = 0x20;
  constexpr unsigned char del = 0x7F;
  if (string.size() > record_string_limit || std::ranges::any_of(string, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < first_printable || byte == del;
      })) {
    throw Problem{std::string(name) + " must be printable and at most " +
                  std::to_string(record_string_limit) + " bytes"};
  }
  return string;
}

bool flag(const Json& value, std::string_view name) {
  if (!value.is_boolean()) {
    throw Problem{std::string(name) + " must be a boolean"};
  }
  return value.get<bool>();
}

// Checks that `value` is an object with exactly the members `known`.
template <typename Names>
void exact_object(const Json& value, std::string_view name, const Names& known) {
  if (!value.is_object()) {
    throw Problem{std::string(name) + " must be an object"};
  }
  for (const auto& [member, unused] : value.items()) {
    if (std::ranges::find(known, member) == std::ranges::end(known)) {
      throw Problem{std::string(name) + ": unknown member '" + member + "'"};
    }
  }
  for (const std::string_view member : known) {
    if (!value.contains(member)) {
      throw Problem{std::string(name) + ": missing member '" + std::string(member) + "'"};
    }
  }
}

void check_list(const Json& value, std::string_view name) {
  if (!value.is_array() || value.size() > record_list_limit) {
    throw Problem{std::string(name) + " must be an array of at most " +
                  std::to_string(record_list_limit) + " entries"};
  }
}

ProcessRef process(const Json& value, std::string_view name) {
  exact_object(value, name, std::initializer_list<std::string_view>{"pid", "image"});
  return ProcessRef{.pid = static_cast<std::uint32_t>(integer(value.at("pid"), name, max_id)),
                    .image = text(value.at("image"), name)};
}

std::optional<ProcessRef> optional_process(const Json& value, std::string_view name) {
  return value.is_null() ? std::nullopt : std::optional<ProcessRef>{process(value, name)};
}

Incident read(const Json& record) {
  exact_object(record, "incident", record_members);
  const std::optional<Chord> chord = parse_chord(text(record.at("chord"), "chord"));
  if (!chord) {
    throw Problem{R"(chord must be "copy" or "paste")"};
  }
  Incident incident{
      .chord = *chord,
      .time = WallTime{std::chrono::seconds{integer(record.at("time"), "time", max_time)}},
      .latency = milliseconds{integer(record.at("latency_ms"), "latency_ms", max_duration_ms)},
      .capped = flag(record.at("capped"), "capped"),
      .input_lag =
          milliseconds{integer(record.at("input_lag_ms"), "input_lag_ms", max_duration_ms)},
      .foreground = optional_process(record.at("foreground"), "foreground"),
      .owner = optional_process(record.at("owner"), "owner"),
      .holder = optional_process(record.at("holder"), "holder"),
      .viewer = optional_process(record.at("viewer"), "viewer")};
  if (const Json& owner = record.at("owner_unanswered_ms"); !owner.is_null()) {
    incident.owner_unanswered =
        milliseconds{integer(owner, "owner_unanswered_ms", max_duration_ms)};
  }
  check_list(record.at("formats"), "formats");
  check_list(record.at("hung"), "hung");
  check_list(record.at("suspects"), "suspects");
  for (const Json& format : record.at("formats")) {
    exact_object(format, "formats",
                 std::initializer_list<std::string_view>{"id", "name", "synthesized"});
    incident.formats.push_back(
        {.id = static_cast<std::uint32_t>(integer(format.at("id"), "formats.id", max_id)),
         .name = text(format.at("name"), "formats.name"),
         .synthesized = flag(format.at("synthesized"), "formats.synthesized")});
  }
  for (const Json& hung : record.at("hung")) {
    incident.hung.push_back(process(hung, "hung"));
  }
  for (const Json& suspect : record.at("suspects")) {
    incident.suspects.push_back(text(suspect, "suspects"));
  }
  if (const Json& cpu = record.at("cpu_percent"); !cpu.is_null()) {
    constexpr double full = 100.0;
    if (!cpu.is_number() || cpu.get<double>() < 0.0 || cpu.get<double>() > full) {
      throw Problem{"cpu_percent must be a number from 0 to 100"};
    }
    incident.cpu_percent = cpu.get<double>();
  }
  if (const Json& load = record.at("memory_load_percent"); !load.is_null()) {
    constexpr std::int64_t full = 100;
    incident.memory_load_percent = static_cast<int>(integer(load, "memory_load_percent", full));
  }
  return incident;
}

}  // namespace

Json to_record(const Incident& incident) {
  Json formats = bounded_list(
      incident.formats,
      [](const ClipboardFormat& format) {
        return Json{
            {"id", format.id}, {"name", bounded(format.name)}, {"synthesized", format.synthesized}};
      },
      [](std::size_t rest) {
        return Json{{"id", 0}, {"name", more_text(rest) + " formats"}, {"synthesized", false}};
      });
  Json hung = bounded_list(
      incident.hung, [](const ProcessRef& process) { return process_record(process); },
      [](std::size_t rest) { return Json{{"pid", 0}, {"image", more_text(rest) + " processes"}}; });
  Json suspects = bounded_list(
      incident.suspects, [](const std::string& suspect) { return Json(bounded(suspect)); },
      [](std::size_t rest) { return Json(more_text(rest)); });
  const std::int64_t time =
      std::chrono::duration_cast<std::chrono::seconds>(incident.time.time_since_epoch()).count();
  return Json{
      {"chord", to_string(incident.chord)},
      {"time", std::clamp<std::int64_t>(time, 0, max_time)},
      {"latency_ms", bounded_ms(incident.latency)},
      {"capped", incident.capped},
      {"owner_unanswered_ms",
       incident.owner_unanswered ? Json(bounded_ms(*incident.owner_unanswered)) : Json(nullptr)},
      {"input_lag_ms", bounded_ms(incident.input_lag)},
      {"foreground", process_record(incident.foreground)},
      {"owner", process_record(incident.owner)},
      {"holder", process_record(incident.holder)},
      {"viewer", process_record(incident.viewer)},
      {"formats", std::move(formats)},
      {"hung", std::move(hung)},
      {"suspects", std::move(suspects)},
      {"cpu_percent",
       incident.cpu_percent ? Json(std::clamp(*incident.cpu_percent, 0.0, 100.0)) : Json(nullptr)},
      {"memory_load_percent", incident.memory_load_percent
                                  ? Json(std::clamp(*incident.memory_load_percent, 0, 100))
                                  : Json(nullptr)}};
}

Result<Incident> parse_record(const Json& record) {
  try {
    return read(record);
  } catch (const Problem& problem) {
    return fail(Errc::invalid_argument, "incident: " + problem.message);
  }
}

}  // namespace myclean::input
