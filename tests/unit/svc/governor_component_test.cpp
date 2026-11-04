#include "svc/governor_component.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/events/event_router.hpp"
#include "core/events/events.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_jobs.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using testing::ComponentHarness;
using testing::eventually;
using testing::shipped_data;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 1 / 2026} + 12h;
constexpr const char* cargo_path = R"(C:\Users\owner\.cargo\bin\cargo.exe)";

// Copies one file of the shipped data folder into `to` (same relative path).
void copy_shipped(const std::filesystem::path& relative, const std::filesystem::path& to) {
  std::ifstream in(shipped_data / relative, std::ios::binary);
  std::filesystem::create_directories((to / relative).parent_path());
  std::ofstream out(to / relative, std::ios::binary);
  std::copy(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(),
            std::ostreambuf_iterator<char>(out));
}

// The governor component over FakeJobs: one FakeJobs for job objects (kept to inspect), a
// second as process control. The router stands in for the events component's.
class GovernorComponentTest : public ::testing::Test {
 public:
  GovernorComponentTest() {
    auto jobs = std::make_unique<fakes::FakeJobs>();
    jobs_ = jobs.get();
    component_ = std::make_unique<GovernorComponent>(
        GovernorSources{.jobs = std::move(jobs), .processes = std::make_unique<fakes::FakeJobs>()},
        files_, &router_);
  }

 protected:
  [[nodiscard]] ComponentHarness& harness() { return harness_; }
  [[nodiscard]] GovernorComponent& component() { return *component_; }
  [[nodiscard]] fakes::FakeJobs& jobs() { return *jobs_; }
  [[nodiscard]] const packs::DiskPackFiles& files() const { return files_; }

  // A data folder with governor.toml in `mode` and the shipped process-classes pack.
  [[nodiscard]] std::filesystem::path data_in(std::string_view mode) const {
    const std::filesystem::path data = harness_.data_dir();
    copy_shipped(std::filesystem::path("packs") / "process-classes" / "pack.toml", data);
    copy_shipped(std::filesystem::path("packs") / "process-classes" / "classes.toml", data);
    std::ofstream(data / "governor.toml") << "[governor]\nmode = \"" << mode << "\"\n";
    return data;
  }
  [[nodiscard]] std::filesystem::path act_data() const { return data_in("act"); }

  // Delivers the ETW start of cargo.exe, a process the job FakeJobs knows.
  void start_cargo() {
    const os::ProcessKey cargo = jobs_->add_process(100, noon - 1h);
    router_.on_start(events::ProcessStart{.process = {.pid = cargo.pid, .created = cargo.created},
                                          .parent_pid = 4,
                                          .session = 1,
                                          .image = cargo_path});
  }

  [[nodiscard]] Json status() { return component_->governor()->status(); }

 private:
  fakes::FakeClock clock_{noon};
  packs::DiskPackFiles files_;
  events::EventRouter router_;
  fakes::FakeJobs* jobs_ = nullptr;
  ComponentHarness harness_{clock_};
  std::unique_ptr<GovernorComponent> component_;
};

TEST_F(GovernorComponentTest, AnObservingPolicyRecordsStartsFromTheEventRouter) {
  ASSERT_TRUE(harness().start(component(), data_in("observe")));
  EXPECT_EQ(status().at("mode"), "observe");
  start_cargo();
  ASSERT_TRUE(eventually([&] {
    const Json current = status();
    return std::ranges::any_of(current.at("events"), [](const Json& event) {
      return event.at("kind") == "place_job" &&
             event.at("detail") == "observe: class_job_then_tree_job";
    });
  }));
  EXPECT_TRUE(status().at("jobs").empty());
}

TEST_F(GovernorComponentTest, ActModePlacesBuildTreesAndStopFailsOpen) {
  ASSERT_TRUE(harness().start(component(), act_data()));
  start_cargo();
  ASSERT_TRUE(eventually([&] { return status().at("jobs").size() == 2U; }));
  std::vector<os::JobRef> refs;
  const Json current = status();
  for (const Json& job : current.at("jobs")) {
    const std::string name = job.at("name");
    const std::string prefix = "Global\\MyClean.Job.build.";
    ASSERT_TRUE(name.starts_with(prefix)) << name;
    refs.push_back(os::JobRef{.job_class = "build", .nonce = name.substr(prefix.size())});
  }
  component().stop();
  for (const os::JobRef& ref : refs) {
    EXPECT_EQ(jobs().job_limits(ref), os::JobLimits{});
    EXPECT_FALSE(jobs().job_exists(ref));
  }
  EXPECT_EQ(testing::query_int(harness().dir() / "myclean.db", "SELECT COUNT(*) FROM governor_job"),
            0);
}

TEST_F(GovernorComponentTest, TrayOnlyMethodsAreRefusedForAnyOtherCaller) {
  ASSERT_TRUE(harness().start(component()));
  const Result<Json> paused = harness().call(ipc::governor_pause_method, Json{{"minutes", 10}});
  ASSERT_FALSE(paused.has_value());
  EXPECT_EQ(paused.error().code, Errc::refused);
  const Result<Json> state = harness().call(ipc::governor_user_state_method,
                                            Json{{"state", "busy"}, {"foreground_pid", 1}});
  ASSERT_FALSE(state.has_value());
  EXPECT_EQ(state.error().code, Errc::refused);
  EXPECT_TRUE(harness().call(ipc::governor_status_method).has_value());
  EXPECT_TRUE(harness().call(ipc::governor_events_method).has_value());
}

TEST_F(GovernorComponentTest, HealthNamesWhatIsMissing) {
  ASSERT_TRUE(harness().start(component()));
  const ipc::ComponentHealth health = component().health();
  EXPECT_EQ(health.name, "governor");
  EXPECT_TRUE(health.healthy) << (health.problems.empty() ? "" : health.problems.front());
  ASSERT_EQ(health.unavailable.size(), 1U);
  EXPECT_NE(health.unavailable[0].find("GPU"), std::string::npos);
}

TEST_F(GovernorComponentTest, StartFailsWithoutJobControl) {
  GovernorComponent incomplete(GovernorSources{}, files(), nullptr);
  EXPECT_FALSE(harness().start(incomplete).has_value());
}

}  // namespace
}  // namespace myclean::svc
