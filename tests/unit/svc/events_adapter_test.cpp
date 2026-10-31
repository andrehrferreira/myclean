#include "svc/events_adapter.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "events/event_fakes.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

struct Sources {
  events::testing::FakeEventSource* events = nullptr;
  events::testing::FakeConnectionTable* connections = nullptr;
  EventSources owned;
};

Sources sources() {
  auto source = std::make_unique<events::testing::FakeEventSource>();
  auto connections = std::make_unique<events::testing::FakeConnectionTable>();
  Sources result{.events = source.get(), .connections = connections.get(), .owned = {}};
  result.owned = EventSources{.events = std::move(source),
                              .connections = std::move(connections),
                              .processes = std::make_unique<events::testing::FakeProcessTable>()};
  return result;
}

TEST(EventsAdapter, RunsTheEventsComponentAndIsHealthy) {
  const fakes::FakeClock clock{noon};
  testing::ComponentHarness harness{clock};
  Sources fakes = sources();
  EventsAdapter adapter{std::move(fakes.owned), nullptr};
  EXPECT_EQ(adapter.name(), "events");
  ASSERT_TRUE(harness.start(adapter).has_value());

  EXPECT_NE(fakes.events->sink(), nullptr);
  const ipc::ComponentHealth health = adapter.health();
  EXPECT_TRUE(health.healthy);
  EXPECT_TRUE(health.problems.empty());
  EXPECT_TRUE(health.unavailable.empty());
  adapter.stop();
  EXPECT_EQ(fakes.events->stops(), 1);
}

TEST(EventsAdapter, EtwUnavailableIsListedAndAFailedSnapshotIsAProblem) {
  const fakes::FakeClock clock{noon};
  testing::ComponentHarness harness{clock};
  Sources fakes = sources();
  fakes.events->fail_with(Error{.code = Errc::access_denied, .message = "not elevated"});
  fakes.connections->set(fail(Errc::io_error, "table unavailable"));
  EventsAdapter adapter{std::move(fakes.owned), nullptr};
  ASSERT_TRUE(harness.start(adapter).has_value());

  const ipc::ComponentHealth health = adapter.health();
  EXPECT_FALSE(health.healthy);
  EXPECT_EQ(health.unavailable, std::vector<std::string>{"ETW unavailable: not elevated"});
  EXPECT_EQ(health.problems, std::vector<std::string>{"connections: table unavailable"});
}

}  // namespace
}  // namespace myclean::svc
