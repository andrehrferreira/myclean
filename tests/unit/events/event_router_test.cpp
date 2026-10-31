#include "core/events/event_router.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/events/events.hpp"
#include "core/telemetry/process_table.hpp"
#include "event_fakes.hpp"

namespace myclean::events {
namespace {

using namespace std::chrono_literals;
using testing::noon;
using testing::RecordingSink;
using testing::start_of;
using testing::transfer_of;

class FixedSigner final : public ISignerLookup {
 public:
  [[nodiscard]] std::optional<std::string> signer(std::string_view image) override {
    if (image.ends_with("cmd.exe")) {
      return "Microsoft Windows";
    }
    return std::nullopt;
  }
};

class EventRouterTest : public ::testing::Test {
 protected:
  void SetUp() override { router_.subscribe(sink_); }

  EventRouter& router() { return router_; }
  RecordingSink& sink() { return sink_; }

 private:
  EventRouter router_;
  RecordingSink sink_;
};

TEST_F(EventRouterTest, ChildOfALiveParentGetsTheParentInstance) {
  router().on_start(start_of(100, noon, 4, R"(C:\Windows\explorer.exe)"));
  router().on_start(start_of(200, noon + 5s, 100, R"(C:\Windows\System32\cmd.exe)"));

  ASSERT_EQ(sink().starts.size(), 2U);
  EXPECT_FALSE(sink().starts[0].parent.has_value());  // PID 4 is not known
  EXPECT_EQ(sink().starts[1].parent, (ProcessKey{.pid = 100, .created = noon}));
  EXPECT_EQ(sink().starts[1].image, R"(C:\Windows\System32\cmd.exe)");
}

TEST_F(EventRouterTest, ReusedParentPidIsNotTakenForTheParent) {
  // The child was created at noon by PID 100, which exited; PID 100 now belongs to a process
  // created after the child.
  router().on_start(start_of(100, noon + 10s, 4, R"(C:\Tools\newer.exe)"));
  router().on_start(start_of(200, noon, 100, R"(C:\Tools\child.exe)"));

  ASSERT_EQ(sink().starts.size(), 2U);
  EXPECT_EQ(sink().starts[1].parent_pid, 100U);
  EXPECT_FALSE(sink().starts[1].parent.has_value());
}

TEST_F(EventRouterTest, ExitedParentIsNotResolved) {
  router().on_start(start_of(100, noon, 4, R"(C:\Tools\parent.exe)"));
  router().on_stop(
      ProcessStop{.process = {.pid = 100, .created = noon}, .exited = noon + 1s, .exit_code = 0});
  router().on_start(start_of(200, noon + 2s, 100, R"(C:\Tools\orphan.exe)"));

  EXPECT_FALSE(sink().starts.back().parent.has_value());
  EXPECT_EQ(router().process(100), nullptr);
  ASSERT_EQ(sink().stops.size(), 1U);
}

TEST_F(EventRouterTest, StopOfAnOlderInstanceKeepsTheCurrentOne) {
  router().on_start(start_of(100, noon + 10s, 4, R"(C:\Tools\current.exe)"));
  // A late stop of the previous owner of PID 100.
  router().on_stop(ProcessStop{.process = {.pid = 100, .created = noon}, .exited = noon + 11s});

  ASSERT_NE(router().process(100), nullptr);
  EXPECT_EQ(router().process(100)->image, R"(C:\Tools\current.exe)");
}

TEST_F(EventRouterTest, SeededProcessesResolveParentsButAreNotDelivered) {
  router().seed(std::vector<telemetry::ProcessInfo>{
      {.pid = 100, .parent_pid = 4, .created = noon - 1h, .image = "explorer.exe"}});
  router().on_start(start_of(200, noon, 100, R"(C:\Windows\System32\cmd.exe)"));

  ASSERT_EQ(sink().starts.size(), 1U);
  EXPECT_EQ(sink().starts[0].parent, (ProcessKey{.pid = 100, .created = noon - 1h}));
}

TEST_F(EventRouterTest, SeedKeepsAProcessAlreadyKnownFromAnEvent) {
  router().on_start(start_of(100, noon, 4, R"(C:\Tools\fresh.exe)"));
  router().seed(std::vector<telemetry::ProcessInfo>{
      {.pid = 100, .parent_pid = 4, .created = noon - 1h, .image = "stale.exe"}});

  EXPECT_EQ(router().process(100)->image, R"(C:\Tools\fresh.exe)");
}

TEST_F(EventRouterTest, TransfersCarryTheirLiveProcess) {
  router().on_start(start_of(100, noon, 4, R"(C:\Tools\uploader.exe)"));
  router().on_network(transfer_of(100, noon + 1s, Direction::send, 1000));
  router().on_network(transfer_of(300, noon + 1s, Direction::receive, 10));

  ASSERT_EQ(sink().transfers.size(), 2U);
  ASSERT_NE(sink().transfers[0].process, nullptr);
  EXPECT_EQ(sink().transfers[0].process->image, R"(C:\Tools\uploader.exe)");
  EXPECT_EQ(sink().transfers[1].process, nullptr);
}

TEST_F(EventRouterTest, UnsubscribedSinkGetsNothingMore) {
  router().unsubscribe(sink());
  router().on_start(start_of(100, noon, 4, R"(C:\Tools\a.exe)"));
  EXPECT_TRUE(sink().starts.empty());
}

TEST(EventRouter, StartsGetTheirSigner) {
  FixedSigner signers;
  EventRouter router(&signers);
  RecordingSink sink;
  router.subscribe(sink);

  router.on_start(start_of(100, noon, 4, R"(C:\Windows\System32\cmd.exe)"));
  router.on_start(start_of(101, noon, 4, R"(C:\Users\me\tool.exe)"));

  ASSERT_EQ(sink.starts.size(), 2U);
  EXPECT_EQ(sink.starts[0].signer, "Microsoft Windows");
  EXPECT_FALSE(sink.starts[1].signer.has_value());
}

}  // namespace
}  // namespace myclean::events
