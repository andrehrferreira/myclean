#include "core/ipc/status.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"

namespace myclean::ipc {
namespace {

using Json = nlohmann::json;

StatusReply sample() {
  return StatusReply{.healthy = true,
                     .version = "20261101-142310-3fa9c2d1",
                     .build = "0.1.0",
                     .mode = "service",
                     .pid = 4242,
                     .uptime_seconds = 90,
                     .store = StoreStatus{.path = R"(C:\ProgramData\MyClean\myclean.db)"},
                     .components = {"proc", "perf"}};
}

TEST(StatusTest, SerializesTheDocumentedMembers) {
  const Json json = to_json(sample());
  EXPECT_EQ(json, Json::parse(R"({
    "health": "ok", "version": "20261101-142310-3fa9c2d1", "build": "0.1.0", "mode": "service",
    "pid": 4242, "uptime_s": 90,
    "store": {"path": "C:\\ProgramData\\MyClean\\myclean.db", "error": null},
    "components": ["proc", "perf"]})"));
}

TEST(StatusTest, RoundTrips) {
  StatusReply degraded = sample();
  degraded.healthy = false;
  degraded.version.reset();
  degraded.store.error = "disk I/O error";
  for (const StatusReply& status : {sample(), degraded}) {
    const Result<StatusReply> parsed = parse_status(to_json(status));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->healthy, status.healthy);
    EXPECT_EQ(parsed->version, status.version);
    EXPECT_EQ(parsed->build, status.build);
    EXPECT_EQ(parsed->mode, status.mode);
    EXPECT_EQ(parsed->pid, status.pid);
    EXPECT_EQ(parsed->uptime_seconds, status.uptime_seconds);
    EXPECT_EQ(parsed->store.path, status.store.path);
    EXPECT_EQ(parsed->store.error, status.store.error);
    EXPECT_EQ(parsed->components, status.components);
  }
}

TEST(StatusTest, RejectsMissingOrMistypedMembers) {
  Json missing = to_json(sample());
  missing.erase("uptime_s");
  Json mistyped = to_json(sample());
  mistyped["pid"] = "4242";
  Json unknown_health = to_json(sample());
  unknown_health["health"] = "fine";
  for (const Json& json : {missing, mistyped, unknown_health, Json::array()}) {
    const Result<StatusReply> parsed = parse_status(json);
    ASSERT_FALSE(parsed.has_value()) << json.dump();
    EXPECT_EQ(parsed.error().code, Errc::invalid_data);
  }
}

}  // namespace
}  // namespace myclean::ipc
