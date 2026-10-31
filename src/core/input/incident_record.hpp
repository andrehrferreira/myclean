#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/model/result.hpp"

// The record form of an Incident: how the tray sends an incident to the service
// (input.record_incident, core/ipc/input.hpp). The service accepts it only through this strict
// schema and stores it as data; no string in it is ever used as a path, command or other OS
// input.
//
//   {"chord": "copy"|"paste", "time": Unix seconds, "latency_ms", "capped": bool,
//    "owner_unanswered_ms": integer|null, "input_lag_ms",
//    "foreground", "owner", "holder", "viewer": process|null,
//    "formats": [{"id", "name", "synthesized": bool}], "hung": [process],
//    "suspects": [image name], "cpu_percent": number|null, "memory_load_percent": integer|null}
//   process = {"pid", "image"}
//
// Every member is required (null for none) and no other member is accepted. Durations are
// 0 to 3 600 000 ms, PIDs and format ids 0 to 2^32-1, percentages 0 to 100, times 0 to
// 9999-12-31; strings are printable (no control character) and at most record_string_limit
// bytes; lists hold at most record_list_limit entries.
namespace myclean::input {

inline constexpr std::size_t record_string_limit = 260;
inline constexpr std::size_t record_list_limit = 64;

// Always fits the schema, so a measured incident is never refused for its size: a string has
// its control characters replaced by '?' and is cut to record_string_limit bytes ending in
// "..."; a list longer than record_list_limit keeps its first record_list_limit - 1 entries and
// ends in a marker entry naming how many were left out ("(5 more)", a format with id 0 named
// "(5 more) formats", a process with pid 0 named "(5 more) processes"); times, durations and
// percentages are clamped to their ranges.
[[nodiscard]] nlohmann::json to_record(const Incident& incident);
// Fails with invalid_argument naming the first member that does not fit the schema.
[[nodiscard]] Result<Incident> parse_record(const nlohmann::json& record);

}  // namespace myclean::input
