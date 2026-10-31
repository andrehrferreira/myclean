#include "core/input/incident_record.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::input {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

Incident full() {
  return Incident{.chord = Chord::paste,
                  .time = noon,
                  .latency = 800ms,
                  .capped = false,
                  .owner_unanswered = 700ms,
                  .input_lag = 16ms,
                  .foreground = ProcessRef{.pid = 10, .image = "Code.exe"},
                  .owner = ProcessRef{.pid = 20, .image = "EXCEL.EXE"},
                  .holder = ProcessRef{.pid = 30, .image = "clip.exe"},
                  .viewer = ProcessRef{.pid = 40, .image = "viewer.exe"},
                  .formats = {{.id = 13, .name = "CF_UNICODETEXT", .synthesized = false},
                              {.id = 1, .name = "CF_TEXT", .synthesized = true}},
                  .hung = {ProcessRef{.pid = 50, .image = "hung.exe"}},
                  .suspects = {"AnyDesk.exe", "Discord.exe"},
                  .cpu_percent = 97.5,
                  .memory_load_percent = 81};
}

TEST(IncidentRecord, RoundTripsEveryField) {
  for (const Incident& incident : {full(), Incident{.time = noon, .latency = 300ms}}) {
    const Json record = to_record(incident);
    const Result<Incident> parsed = parse_record(record);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message << "\n" << record.dump();
    EXPECT_EQ(*parsed, incident);
  }
}

TEST(IncidentRecord, HasTheDocumentedShape) {
  const Json record = to_record(Incident{.chord = Chord::copy, .time = noon, .latency = 300ms});
  EXPECT_EQ(record, (Json{{"chord", "copy"},
                          {"time", 1'793'448'000},
                          {"latency_ms", 300},
                          {"capped", false},
                          {"owner_unanswered_ms", nullptr},
                          {"input_lag_ms", 0},
                          {"foreground", nullptr},
                          {"owner", nullptr},
                          {"holder", nullptr},
                          {"viewer", nullptr},
                          {"formats", Json::array()},
                          {"hung", Json::array()},
                          {"suspects", Json::array()},
                          {"cpu_percent", nullptr},
                          {"memory_load_percent", nullptr}}));
}

TEST(IncidentRecord, AnOversizedIncidentIsBoundedToTheSchemaNotRefused) {
  Incident incident = full();
  incident.formats.clear();
  for (std::uint32_t id = 1; id <= 70; ++id) {
    incident.formats.push_back({.id = id, .name = "format"});
  }
  incident.hung = std::vector<ProcessRef>(100, ProcessRef{.pid = 7, .image = "hung.exe"});
  incident.suspects = std::vector<std::string>(65, "suspect.exe");
  // 300 bytes: 'a' then 'é' (2 bytes) repeated, so the cut must not split a character.
  std::string long_name = "a";
  while (long_name.size() < 300) {
    long_name += "\xC3\xA9";
  }
  incident.owner = ProcessRef{.pid = 20, .image = long_name};
  incident.foreground = ProcessRef{.pid = 10, .image = "tab\there"};
  incident.latency = std::chrono::hours{2};
  incident.cpu_percent = 100.4;

  const Json record = to_record(incident);
  const Result<Incident> parsed = parse_record(record);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

  ASSERT_EQ(parsed->formats.size(), record_list_limit);
  EXPECT_EQ(parsed->formats[62].id, 63U);
  EXPECT_EQ(parsed->formats.back(), (ClipboardFormat{.id = 0, .name = "(7 more) formats"}));
  ASSERT_EQ(parsed->hung.size(), record_list_limit);
  EXPECT_EQ(parsed->hung.back(), (ProcessRef{.pid = 0, .image = "(37 more) processes"}));
  ASSERT_EQ(parsed->suspects.size(), record_list_limit);
  EXPECT_EQ(parsed->suspects.back(), "(2 more)");

  const std::string owner = parsed->owner.value_or(ProcessRef{}).image;
  EXPECT_LE(owner.size(), record_string_limit);
  EXPECT_TRUE(owner.ends_with("..."));
  EXPECT_TRUE(owner.starts_with(long_name.substr(0, owner.size() - 3)));
  EXPECT_NO_THROW(static_cast<void>(Json(owner).dump()));  // still valid UTF-8
  EXPECT_EQ(parsed->foreground.value_or(ProcessRef{}).image, "tab?here");
  EXPECT_EQ(parsed->latency, std::chrono::hours{1});
  EXPECT_EQ(parsed->cpu_percent, 100.0);

  // Exactly at the limits nothing is cut.
  Incident limit = full();
  limit.suspects =
      std::vector<std::string>(record_list_limit, std::string(record_string_limit, 's'));
  EXPECT_EQ(parse_record(to_record(limit))->suspects, limit.suspects);
}

TEST(IncidentRecord, RejectsAnythingOutsideTheSchema) {
  const auto with = [](const std::string& member, Json value) {
    Json record = to_record(full());
    record[member] = std::move(value);
    return record;
  };
  Json missing = to_record(full());
  missing.erase("hung");
  Json nested = to_record(full());
  nested["owner"]["path"] = R"(C:\x.exe)";
  const std::vector<std::pair<Json, std::string>> cases{
      {with("command", "calc"), "unknown member 'command'"},
      {missing, "missing member 'hung'"},
      {nested, "owner: unknown member 'path'"},
      {with("chord", "cut"), "chord"},
      {with("latency_ms", -5), "latency_ms"},
      {with("latency_ms", 3'600'001), "latency_ms"},
      {with("latency_ms", 1.5), "latency_ms"},
      {with("time", "noon"), "time"},
      {with("capped", 1), "capped"},
      {with("cpu_percent", 101.0), "cpu_percent"},
      {with("memory_load_percent", 101), "memory_load_percent"},
      {with("suspects", Json::array({"tab\there"})), "suspects"},
      {with("suspects", Json::array({std::string(261, 'x')})), "suspects"},
      {with("suspects", Json(std::vector<std::string>(65, "a.exe"))), "suspects"},
      {with("formats", Json::array({{{"id", 13}, {"name", "x"}}})), "formats"},
      {with("hung", Json::array({{{"pid", 4'294'967'296}, {"image", "x"}}})), "hung"},
      {Json::array(), "object"},
  };
  for (const auto& [record, expected] : cases) {
    const Result<Incident> parsed = parse_record(record);
    ASSERT_FALSE(parsed.has_value()) << record.dump();
    EXPECT_EQ(parsed.error().code, Errc::invalid_argument);
    EXPECT_NE(parsed.error().message.find(expected), std::string::npos)
        << parsed.error().message << " (expected " << expected << ")";
  }
}

}  // namespace
}  // namespace myclean::input
