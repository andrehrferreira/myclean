// The cleaning component in the service: its threads over fakes, and the tray-only guard on the
// real pipe, where the caller is identified by the pipe client's process.

#include "svc/cleaning_component.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "cleaning/cleaning_fixture.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"
#include "packs/pack_fakes.hpp"
#include "phase1_fakes.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "platform/ipc/security.hpp"
#include "platform_fs/sandbox.hpp"
#include "svc/owner_session.hpp"
#include "svc/service.hpp"
#include "svc_test_support.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using namespace myclean::literals;
namespace fs = std::filesystem;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

fs::path this_executable() {
  std::array<wchar_t, 32768> buffer{};
  const DWORD length =
      ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  return {std::wstring(buffer.data(), length)};
}

bool interactive_session() {
  DWORD session = 0;
  return ::ProcessIdToSessionId(::GetCurrentProcessId(), &session) != FALSE && session != 0;
}

// A cleaning component over an in-memory file system holding F:\cache\s1.bin.
std::unique_ptr<CleaningComponent> fake_cleaning(const packs::IPackFiles& files) {
  auto file_system = std::make_unique<fakes::FakeFileSystem>();
  file_system->add_file(L"F:\\cache\\s1.bin", 1_MiB);
  auto tree =
      std::make_unique<cleaning::testing::FsTree>(*file_system, noon - std::chrono::days{30});
  CleaningSources sources{.file_system = std::move(file_system),
                          .tree = std::move(tree),
                          .usage = std::make_unique<clean::testing::FakeUsage>(),
                          .owner = [](std::string_view) -> Result<cleaning::OwnerContext> {
                            return cleaning::OwnerContext{
                                .environment = std::make_unique<clean::testing::FakeEnvironment>(),
                                .locations = actions::testing::owner_locations(),
                                .downloads = std::nullopt};
                          }};
  return std::make_unique<CleaningComponent>(std::move(sources), files, CleaningMode{});
}

TEST(CleaningComponentTest, ScansOnItsOwnThreadAndRegistersThePlanMethods) {
  const fakes::FakeClock clock{noon};
  testing::ComponentHarness harness{clock};
  packs::testing::FakePackFiles files;
  cleaning::testing::put_test_pack(files);
  const std::unique_ptr<CleaningComponent> component = fake_cleaning(files);
  ASSERT_TRUE(harness.start(*component, cleaning::testing::data_dir).has_value());
  ASSERT_TRUE(testing::eventually([&] {
    const Result<Json> status = harness.call("cleaning.status");
    return status && status->at("unavailable").is_null();
  }));
  EXPECT_EQ(harness.code("cleaning.scan", Json::object()), ipc::rpc_error::server_error);
  EXPECT_EQ(harness.call("cleaning.scan", Json::object(), ipc::Caller{.tray = true}).value(),
            (Json{{"started", true}}));
  ASSERT_TRUE(testing::eventually([&] {
    const Result<Json> plans = harness.call("plans.list");
    return plans && plans->at("plans").size() == 1;
  }));
  EXPECT_EQ(harness.call("plans.list")->at("plans")[0].at("state"), "awaiting_approval");
  EXPECT_TRUE(component->health().healthy);
  // The harness is no tray: the approval is refused.
  EXPECT_EQ(harness.code("plans.approve", Json{{"id", 1}, {"selection", {{"all", true}}}}),
            ipc::rpc_error::server_error);
  component->stop();
}

TEST(OwnerTrayTest, OnlyTheConfiguredImageOfTheOwnerInAnInteractiveSessionIsTheTray) {
  const Result<std::string> owner = platform::current_user_sid();
  ASSERT_TRUE(owner.has_value());
  const std::uint32_t self = ::GetCurrentProcessId();
  EXPECT_FALSE(is_owner_tray(self, *owner, L"C:\\Program Files\\MyClean\\myclean-tray.exe"));
  EXPECT_FALSE(is_owner_tray(self, "S-1-5-18", this_executable()));
  EXPECT_FALSE(is_owner_tray(0, *owner, this_executable()));
  EXPECT_FALSE(is_owner_tray(self, *owner, {}));
  if (!interactive_session()) {
    GTEST_SKIP() << "the tests run in session 0, where no tray runs";
  }
  EXPECT_TRUE(is_owner_tray(self, *owner, this_executable()));
}

// The deploy starts the tray through C:\Program Files\MyClean\current, a junction to the
// version folder, while Windows reports the image under the version folder itself.
TEST(OwnerTrayTest, TheTrayImageMayBeNamedThroughAJunction) {
  const Result<std::string> owner = platform::current_user_sid();
  ASSERT_TRUE(owner.has_value());
  if (!interactive_session()) {
    GTEST_SKIP() << "the tests run in session 0, where no tray runs";
  }
  const platform::Sandbox sandbox;
  const fs::path current = sandbox.root() / "current";
  ASSERT_TRUE(platform::make_junction(current, this_executable().parent_path()));
  const std::uint32_t self = ::GetCurrentProcessId();
  EXPECT_TRUE(is_owner_tray(self, *owner, current / this_executable().filename()));
  EXPECT_FALSE(is_owner_tray(self, *owner, current / "myclean-tray.exe"));
}

class PipeGuardTest : public ::testing::Test {
 protected:
  // A console service with the cleaning component, whose tray is `tray_image`.
  [[nodiscard]] std::unique_ptr<Service> start(const fs::path& tray_image) {
    const Result<std::string> owner = platform::current_user_sid();
    EXPECT_TRUE(owner.has_value());
    cleaning::testing::put_test_pack(files_);
    std::vector<std::unique_ptr<IComponent>> components;
    components.push_back(fake_cleaning(files_));
    Result<std::unique_ptr<Service>> service =
        Service::start(ServiceOptions{.database = dir_.path() / "myclean.db",
                                      .data_dir = cleaning::testing::data_dir,
                                      .pipe_name = pipe_,
                                      .owner_sid = owner.value_or(""),
                                      .mode = "console",
                                      .version = std::nullopt,
                                      .tray_image = tray_image},
                       std::move(components), clock_);
    EXPECT_TRUE(service.has_value()) << service.error().message;
    return service ? std::move(*service) : nullptr;
  }

  // The Errc of plans.reject of a plan that does not exist, called over the pipe.
  [[nodiscard]] Errc reject_unknown_plan() const {
    Result<platform::PipeClient> client = platform::PipeClient::connect(pipe_, 5s);
    EXPECT_TRUE(client.has_value());
    if (!client) {
      return Errc::io_error;
    }
    const Result<Json> answer = client->call("plans.reject", Json{{"id", 4242}});
    EXPECT_FALSE(answer.has_value());
    return answer ? Errc::io_error : answer.error().code;
  }

 private:
  testing::TempDir dir_;
  fakes::FakeClock clock_{noon};
  std::wstring pipe_ = testing::unique_pipe_name();
  packs::testing::FakePackFiles files_;
};

TEST_F(PipeGuardTest, AnotherProcessImageIsRefusedBeforeAnythingRuns) {
  const std::unique_ptr<Service> service = start(L"C:\\Program Files\\MyClean\\myclean-tray.exe");
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(reject_unknown_plan(), Errc::refused);
}

TEST_F(PipeGuardTest, TheConfiguredTrayPassesTheGuard) {
  if (!interactive_session()) {
    GTEST_SKIP() << "the tests run in session 0, where no tray runs";
  }
  const std::unique_ptr<Service> service = start(this_executable());
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(reject_unknown_plan(), Errc::not_found);  // the guard let it through
}

}  // namespace
}  // namespace myclean::svc
