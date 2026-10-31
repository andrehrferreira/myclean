// End to end: myclean-svc.exe in console mode on a unique pipe, with a cleaning sandbox. Its
// only rules select files inside the sandbox, its quarantine is inside the sandbox, and a
// console run never applies Windows handlers, DISM or system settings, so nothing outside the
// sandbox can be touched. This test process is the configured tray.

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "platform/ipc/pipe_client.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

fs::path this_executable() {
  std::array<wchar_t, 32768> buffer{};
  const DWORD length =
      ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  return {std::wstring(buffer.data(), length)};
}

void write(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

class CleaningSmokeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    DWORD session = 0;
    if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session) == FALSE || session == 0) {
      GTEST_SKIP() << "the tests run in session 0, where no tray runs";
    }
    const std::string unique =
        std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount64());
    sandbox_ = fs::temp_directory_path() / "myclean-it" / ("smoke-" + unique);
    pipe_ = L"myclean-it-" + std::wstring(unique.begin(), unique.end());
    write(sandbox_ / "items" / "old.bin", std::string(4096, 'o'));
    write(sandbox_ / "items" / "keep.bin", std::string(4096, 'k'));
    fs::create_directories(sandbox_ / "ws");
    write(sandbox_ / "data" / "packs" / "sandbox" / "pack.toml",
          "id = \"sandbox\"\nversion = \"1.0.0\"\ntitle = \"Sandbox\"\n"
          "description = \"The smoke test's case.\"\nsources = [\"observed on reference "
          "machine\"]\nmin_myclean = \"0.1.0\"\n");
    write(sandbox_ / "data" / "packs" / "sandbox" / "clean.toml",
          "[[rule]]\nid = \"ask\"\ncategory = \"sandbox\"\ntier = \"ask\"\n"
          "scope = \"user\"\nstrategy = \"quarantine\"\ntargets = ['" +
              utf8(sandbox_ / "items" / "*") + "']\n");
    write(sandbox_ / "data" / "workspaces.toml", "roots = ['" + utf8(sandbox_ / "ws") + "']\n");

    std::wstring command = L"\"" + fs::path(MYCLEAN_SVC_EXE).make_preferred().wstring() +
                           L"\" --console --state \"" + (sandbox_ / "state").wstring() +
                           L"\" --pipe " + pipe_ + L" --sandbox \"" + sandbox_.wstring() +
                           L"\" --tray \"" + this_executable().wstring() + L"\"";
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started{};
    ASSERT_NE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &started),
              FALSE);
    process_.reset(started.hProcess);
    ::CloseHandle(started.hThread);
  }

  void TearDown() override {
    if (process_) {
      ::TerminateProcess(process_.get(), 0);
      ::WaitForSingleObject(process_.get(), 30'000);
    }
    std::error_code ignored;
    fs::remove_all(sandbox_, ignored);
  }

  // Calls `method` on the child service, retrying the connection while it starts.
  Result<Json> call(std::string_view method, const Json& params = Json::object()) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    for (;;) {
      Result<platform::PipeClient> client =
          platform::PipeClient::connect(LR"(\\.\pipe\)" + pipe_, 5s);
      if (client) {
        return client->call(method, params);
      }
      if (std::chrono::steady_clock::now() > deadline) {
        return tl::unexpected(client.error());
      }
      std::this_thread::sleep_for(200ms);
    }
  }

  // Polls `method` until `done` holds for its answer (60 s at most).
  std::optional<Json> until(std::string_view method, const Json& params,
                            const std::function<bool(const Json&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (Result<Json> answer = call(method, params); answer && done(*answer)) {
        return *answer;
      }
      std::this_thread::sleep_for(200ms);
    }
    return std::nullopt;
  }

  [[nodiscard]] const fs::path& sandbox() const { return sandbox_; }

 private:
  fs::path sandbox_;
  std::wstring pipe_;
  wil::unique_handle process_;
};

TEST_F(CleaningSmokeTest, ScansApprovesQuarantinesAndRestoresInsideTheSandbox) {
  ASSERT_TRUE(until("cleaning.status", Json::object(),
                    [](const Json& status) { return status.at("unavailable").is_null(); }));
  ASSERT_EQ(call("cleaning.scan").value(), (Json{{"started", true}}));
  const std::optional<Json> listed = until(
      "plans.list", Json::object(), [](const Json& list) { return !list.at("plans").empty(); });
  ASSERT_TRUE(listed.has_value());
  const Json plan = listed.value_or(Json{{"plans", {Json::object()}}}).at("plans")[0];
  EXPECT_EQ(plan.at("state"), "awaiting_approval");
  EXPECT_EQ(plan.at("items"), 2);
  const std::int64_t id = plan.at("id").get<std::int64_t>();

  const Json items = call("plans.get", Json{{"id", id}}).value().at("items");
  std::int64_t old_item = 0;
  for (const Json& item : items) {
    if (item.at("path").get<std::string>().ends_with("old.bin")) {
      old_item = item.at("id").get<std::int64_t>();
    }
  }
  ASSERT_NE(old_item, 0);
  ASSERT_EQ(call("plans.approve",
                 Json{{"id", id}, {"selection", {{"all", false}, {"items", {old_item}}}}})
                .value(),
            (Json{{"state", "approved"}}));
  ASSERT_TRUE(until("plans.get", Json{{"id", id}},
                    [](const Json& got) { return got.at("state") == "verified"; }));

  const fs::path held = sandbox() / "quarantine" / ".myclean-quarantine" / std::to_string(id) /
                        std::to_string(old_item) / "old.bin";
  EXPECT_FALSE(fs::exists(sandbox() / "items" / "old.bin"));
  EXPECT_TRUE(fs::exists(held));
  EXPECT_TRUE(fs::exists(sandbox() / "items" / "keep.bin"));

  EXPECT_EQ(call("quarantine.restore", Json{{"plan", id}}).value(), (Json{{"restored", 1}}));
  EXPECT_TRUE(fs::exists(sandbox() / "items" / "old.bin"));
  EXPECT_FALSE(fs::exists(held));
}

}  // namespace
}  // namespace myclean::svc
