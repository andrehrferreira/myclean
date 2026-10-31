#include "svc/service.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "fakes/fake_clock.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ipc/security.hpp"
#include "svc/component.hpp"
#include "svc_test_support.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using testing::query_int;
using testing::rpc_code;
using testing::TempDir;

// A component that owns one table, queues a write into it at start, serves "<name>.ping" and
// records its start and stop calls.
class SampleComponent final : public IComponent {
 public:
  SampleComponent(std::string name, std::vector<std::string>& calls, bool fails = false)
      : name_(std::move(name)), calls_(&calls), fails_(fails) {}

  [[nodiscard]] std::string_view name() const override { return name_; }

  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override {
    return {store::MigrationSet{
        .component = name_,
        .migrations = {{.version = 1,
                        .name = "sample",
                        .sql = "CREATE TABLE " + name_ + "_sample(value INTEGER NOT NULL);"}}}};
  }

  [[nodiscard]] Result<void> start(const Context& context) override {
    calls_->push_back("start " + name_);
    if (fails_) {
      return fail(Errc::not_found, "sensor missing");
    }
    context.store().enqueue("INSERT INTO " + name_ + "_sample(value) VALUES(?1)",
                            {std::int64_t{42}});
    return context.methods().add(name_ + ".ping", {},
                                 [](const Json&) -> Result<Json> { return Json("pong"); });
  }

  void stop() override { calls_->push_back("stop " + name_); }

 private:
  std::string name_;
  std::vector<std::string>* calls_;
  bool fails_;
};

class ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const Result<std::string> user = platform::current_user_sid();
    ASSERT_TRUE(user.has_value());
    owner_ = *user;
  }

  [[nodiscard]] ServiceOptions options() const {
    return ServiceOptions{.database = dir_.path() / "myclean.db",
                          .data_dir = dir_.path() / "data",
                          .pipe_name = pipe_,
                          .owner_sid = owner_,
                          .mode = "console",
                          .version = "20261101-142310-3fa9c2d1"};
  }

  [[nodiscard]] Result<std::unique_ptr<Service>> start(
      std::vector<std::unique_ptr<IComponent>> components) {
    return Service::start(options(), std::move(components), clock_);
  }

  [[nodiscard]] std::unique_ptr<Service> start_sample() {
    std::vector<std::unique_ptr<IComponent>> components;
    components.push_back(std::make_unique<SampleComponent>("alpha", calls_));
    Result<std::unique_ptr<Service>> service = start(std::move(components));
    EXPECT_TRUE(service.has_value()) << service.error().message;
    return service ? std::move(*service) : nullptr;
  }

  [[nodiscard]] platform::PipeClient connect() const {
    Result<platform::PipeClient> client = platform::PipeClient::connect(pipe_, 5s);
    EXPECT_TRUE(client.has_value()) << client.error().message;
    return std::move(*client);
  }

  [[nodiscard]] const TempDir& dir() const { return dir_; }
  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] const std::wstring& pipe() const { return pipe_; }
  [[nodiscard]] const std::string& owner() const { return owner_; }
  [[nodiscard]] std::vector<std::string>& calls() { return calls_; }

 private:
  TempDir dir_;
  fakes::FakeClock clock_{std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h};
  std::wstring pipe_ = testing::unique_pipe_name();
  std::string owner_;
  std::vector<std::string> calls_;
};

TEST_F(ServiceTest, AnswersStatusOverThePipe) {
  const std::unique_ptr<Service> service = start_sample();
  ASSERT_NE(service, nullptr);
  clock().advance(90s);

  platform::PipeClient client = connect();
  const Result<ipc::StatusReply> status = client.status();
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_TRUE(status->healthy);
  EXPECT_EQ(status->version, "20261101-142310-3fa9c2d1");
  EXPECT_EQ(status->build, MYCLEAN_TEST_VERSION);
  EXPECT_EQ(status->mode, "console");
  EXPECT_EQ(status->pid, static_cast<std::int64_t>(::GetCurrentProcessId()));
  EXPECT_EQ(status->uptime_seconds, 90);
  const std::u8string database = (dir().path() / "myclean.db").u8string();
  EXPECT_EQ(status->store.path, std::string(database.begin(), database.end()));
  EXPECT_FALSE(status->store.error.has_value());
  EXPECT_EQ(status->components, std::vector<std::string>{"alpha"});
}

TEST_F(ServiceTest, ServesComponentMethodsAndRefusesEverythingElse) {
  const std::unique_ptr<Service> service = start_sample();
  ASSERT_NE(service, nullptr);
  platform::PipeClient client = connect();

  const Result<Json> pong = client.call("alpha.ping");
  ASSERT_TRUE(pong.has_value()) << pong.error().message;
  EXPECT_EQ(*pong, "pong");

  EXPECT_EQ(rpc_code(client.exchange(R"({"jsonrpc":"2.0","id":1,"method":"run","params":{}})")),
            ipc::rpc_error::method_not_found);
  EXPECT_EQ(rpc_code(client.exchange(
                R"({"jsonrpc":"2.0","id":2,"method":"status","params":{"path":"C:\\Windows"}})")),
            ipc::rpc_error::invalid_params);
  EXPECT_EQ(rpc_code(client.exchange(
                R"({"jsonrpc":"2.0","id":3,"method":"status","params":{"cmd":"cmd /c del"}})")),
            ipc::rpc_error::invalid_params);
}

TEST_F(ServiceTest, StopClosesThePipeStopsComponentsAndFlushesTheStore) {
  std::unique_ptr<Service> service = start_sample();
  ASSERT_NE(service, nullptr);
  ASSERT_TRUE(service->stop().has_value());
  EXPECT_EQ(calls(), (std::vector<std::string>{"start alpha", "stop alpha"}));
  EXPECT_TRUE(service->stop().has_value());  // idempotent
  EXPECT_EQ(calls().size(), 2U);

  const Result<platform::PipeClient> late = platform::PipeClient::connect(pipe(), 1s);
  ASSERT_FALSE(late.has_value());
  EXPECT_EQ(late.error().code, Errc::not_found);

  // The write queued at start was never ticked; stop flushed it.
  service.reset();
  EXPECT_EQ(query_int(dir().path() / "myclean.db", "SELECT COUNT(*) FROM alpha_sample"), 1);
}

TEST_F(ServiceTest, FailsToStartWhenThePipeIsTaken) {
  const Result<std::unique_ptr<platform::PipeServer>> squatter = platform::PipeServer::start(
      platform::PipeServerOptions{.name = pipe(),
                                  .owner_sid = owner(),
                                  .handler = [](std::string_view) { return std::string(); },
                                  .on_error = {}});
  ASSERT_TRUE(squatter.has_value());

  std::vector<std::unique_ptr<IComponent>> components;
  components.push_back(std::make_unique<SampleComponent>("alpha", calls()));
  const Result<std::unique_ptr<Service>> service = start(std::move(components));
  ASSERT_FALSE(service.has_value());
  EXPECT_EQ(service.error().code, Errc::in_use);
  // Nothing is left half-running: the component was stopped and the store flushed and closed.
  EXPECT_EQ(calls(), (std::vector<std::string>{"start alpha", "stop alpha"}));
  EXPECT_EQ(query_int(dir().path() / "myclean.db", "SELECT COUNT(*) FROM alpha_sample"), 1);
}

TEST_F(ServiceTest, AFailingComponentStopsTheStart) {
  std::vector<std::unique_ptr<IComponent>> components;
  components.push_back(std::make_unique<SampleComponent>("alpha", calls()));
  components.push_back(std::make_unique<SampleComponent>("beta", calls(), true));
  components.push_back(std::make_unique<SampleComponent>("gamma", calls()));
  const Result<std::unique_ptr<Service>> service = start(std::move(components));
  ASSERT_FALSE(service.has_value());
  EXPECT_EQ(service.error().code, Errc::not_found);
  EXPECT_EQ(service.error().message, "component beta failed to start: sensor missing");
  EXPECT_EQ(calls(), (std::vector<std::string>{"start alpha", "start beta", "stop alpha"}));

  const Result<platform::PipeClient> client = platform::PipeClient::connect(pipe(), 1s);
  ASSERT_FALSE(client.has_value());
  EXPECT_EQ(client.error().code, Errc::not_found);
}

TEST_F(ServiceTest, FailsToStartOnAnUnusableDatabase) {
  std::filesystem::create_directories(dir().path() / "myclean.db");  // a folder, not a file
  const Result<std::unique_ptr<Service>> service = start({});
  EXPECT_FALSE(service.has_value());
}

// A component whose queued write cannot succeed, to make a store flush fail.
class BrokenWriter final : public IComponent {
 public:
  [[nodiscard]] std::string_view name() const override { return "broken"; }
  [[nodiscard]] Result<void> start(const Context& context) override {
    context.store().enqueue("INSERT INTO no_such_table(value) VALUES(1)", {});
    return {};
  }
  void stop() override {}
};

TEST_F(ServiceTest, ReportsAFailedFlushAsDegradedUntilAWriteSucceeds) {
  std::vector<std::unique_ptr<IComponent>> components;
  components.push_back(std::make_unique<BrokenWriter>());
  Result<std::unique_ptr<Service>> service = start(std::move(components));
  ASSERT_TRUE(service.has_value()) << service.error().message;

  clock().advance(11s);  // past the store's flush interval
  (*service)->tick();
  const ipc::StatusReply degraded = (*service)->status();
  EXPECT_FALSE(degraded.healthy);
  EXPECT_NE(degraded.store.error.value_or("").find("no_such_table"), std::string::npos);

  clock().advance(11s);
  (*service)->tick();  // the failed batch was dropped; this flush succeeds
  EXPECT_TRUE((*service)->status().healthy);
}

}  // namespace
}  // namespace myclean::svc
