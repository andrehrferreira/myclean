#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <wil/resource.h>
#include <windows.h>

#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::platform {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

// A SID no account on this machine has.
constexpr std::string_view stranger = "S-1-5-21-1000000000-2000000000-3000000000-1001";

// Never the real \\.\pipe\myclean.
std::wstring unique_pipe_name() {
  static int counter = 0;
  return LR"(\\.\pipe\myclean-test-)" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(++counter);
}

int rpc_code(const Result<std::string>& response) {
  EXPECT_TRUE(response.has_value());
  return response ? Json::parse(*response).at("error").at("code").get<int>() : 0;
}

class PipeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const Result<std::string> user = current_user_sid();
    ASSERT_TRUE(user.has_value());
    owner_ = *user;
    ASSERT_TRUE(SUCCEEDED(release_.create(wil::EventOptions::ManualReset)));
    ASSERT_TRUE(
        methods_
            .add("echo",
                 {ipc::Param{.name = "text", .type = ipc::ParamType::token, .required = true}},
                 [](const Json& params) -> Result<Json> {
                   return Json{{"text", params.at("text")}};
                 })
            .has_value());
    // Blocks until the test releases it.
    ASSERT_TRUE(methods_
                    .add("wait", {},
                         [this](const Json&) -> Result<Json> {
                           ::WaitForSingleObject(release_.get(), 10'000);
                           return Json(true);
                         })
                    .has_value());
  }

  void TearDown() override {
    release_.SetEvent();
    server_.reset();
  }

  [[nodiscard]] Result<std::unique_ptr<PipeServer>> start_server(std::string owner) {
    return PipeServer::start(PipeServerOptions{
        .name = name_,
        .owner_sid = std::move(owner),
        .handler = [this](std::string_view request) { return methods_.handle_frame(request); },
        .on_error = {}});
  }

  void start() {
    Result<std::unique_ptr<PipeServer>> server = start_server(owner_);
    ASSERT_TRUE(server.has_value()) << server.error().message;
    server_ = std::move(*server);
  }

  [[nodiscard]] PipeClient connect(std::chrono::milliseconds timeout = 5s) const {
    Result<PipeClient> client = PipeClient::connect(name_, timeout);
    EXPECT_TRUE(client.has_value()) << client.error().message;
    return std::move(*client);
  }

  // A raw client handle that may only read the pipe's security.
  [[nodiscard]] wil::unique_hfile open_for_security() const {
    wil::unique_hfile pipe(
        ::CreateFileW(name_.c_str(), READ_CONTROL, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    EXPECT_TRUE(pipe.is_valid()) << "Win32 error " << ::GetLastError();
    return pipe;
  }

  [[nodiscard]] const std::wstring& name() const { return name_; }
  [[nodiscard]] const std::string& owner() const { return owner_; }
  [[nodiscard]] wil::unique_event_nothrow& release() { return release_; }
  [[nodiscard]] std::unique_ptr<PipeServer>& server() { return server_; }

 private:
  std::wstring name_ = unique_pipe_name();
  std::string owner_;
  wil::unique_event_nothrow release_;
  ipc::Dispatcher methods_;
  std::unique_ptr<PipeServer> server_;
};

TEST_F(PipeTest, CallsMethodsOverThePipe) {
  start();
  PipeClient client = connect();
  for (const std::string_view text : {"hello", "again"}) {
    const Result<Json> echoed = client.call("echo", {{"text", text}});
    ASSERT_TRUE(echoed.has_value()) << echoed.error().message;
    EXPECT_EQ(echoed->at("text").get<std::string>(), text);
  }
}

TEST_F(PipeTest, AnswersUnknownMethodsAndRawPathsWithJsonRpcErrors) {
  start();
  PipeClient client = connect();
  EXPECT_EQ(rpc_code(client.exchange(R"({"jsonrpc":"2.0","id":1,"method":"file.delete"})")),
            ipc::rpc_error::method_not_found);
  EXPECT_EQ(rpc_code(client.exchange(
                R"({"jsonrpc":"2.0","id":2,"method":"echo","params":{"text":"C:\\Windows"}})")),
            ipc::rpc_error::invalid_params);

  const Result<Json> smuggled =
      client.call("echo", {{"text", "hi"}, {"path", R"(C:\Users\owner\Documents)"}});
  ASSERT_FALSE(smuggled.has_value());
  EXPECT_EQ(smuggled.error().code, Errc::invalid_argument);
  EXPECT_TRUE(smuggled.error().message.starts_with("json-rpc error -32602"))
      << smuggled.error().message;
}

TEST_F(PipeTest, DaclAllowsOnlySystemAndTheServingOwner) {
  start();
  const wil::unique_hfile pipe = open_for_security();
  ASSERT_TRUE(pipe.is_valid());
  const Result<std::vector<AllowedAce>> aces = object_aces(pipe.get());
  ASSERT_TRUE(aces.has_value()) << aces.error().message;
  ASSERT_EQ(aces->size(), 2U);
  EXPECT_EQ((*aces)[0].sid, system_sid);
  EXPECT_EQ((*aces)[0].mask, static_cast<DWORD>(FILE_ALL_ACCESS));
  EXPECT_EQ((*aces)[1].sid, owner());
  EXPECT_EQ((*aces)[1].mask, static_cast<DWORD>(FILE_ALL_ACCESS));
}

TEST_F(PipeTest, AnotherAccountIsDenied) {
  // Served for a different owner, the pipe admits SYSTEM and that owner only, not this account.
  Result<std::unique_ptr<PipeServer>> started = start_server(std::string(stranger));
  ASSERT_TRUE(started.has_value()) << started.error().message;
  server() = std::move(*started);

  const Result<PipeClient> client = PipeClient::connect(name(), 1s);
  ASSERT_FALSE(client.has_value());
  EXPECT_EQ(client.error().code, Errc::access_denied);
  // Not even its security can be read; security_test.cpp checks that descriptor's ACEs.
  const wil::unique_hfile pipe(
      ::CreateFileW(name().c_str(), READ_CONTROL, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  EXPECT_FALSE(pipe.is_valid());
}

TEST_F(PipeTest, RefusesAnInvalidOwner) {
  const Result<std::unique_ptr<PipeServer>> server = start_server("S-1-5-18)(A;;FA;;;WD");
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error().code, Errc::invalid_argument);
}

TEST_F(PipeTest, ASecondServerOnTheSameNameFails) {
  start();
  const Result<std::unique_ptr<PipeServer>> second = start_server(owner());
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, Errc::in_use);
  // The first server keeps working.
  PipeClient client = connect();
  EXPECT_TRUE(client.call("echo", {{"text", "still-here"}}).has_value());
}

TEST_F(PipeTest, StopDisconnectsClientsAndFreesTheName) {
  start();
  PipeClient idle = connect();
  ASSERT_TRUE(idle.call("echo", {{"text", "x"}}).has_value());
  server()->stop();
  EXPECT_FALSE(idle.call("echo", {{"text", "x"}}).has_value());

  const Result<PipeClient> late = PipeClient::connect(name(), 1s);
  ASSERT_FALSE(late.has_value());
  EXPECT_EQ(late.error().code, Errc::not_found);
}

TEST_F(PipeTest, ServesClientsConcurrently) {
  start();
  PipeClient waiting = connect();
  Result<Json> waited = fail(Errc::invalid_state, "not called");
  std::thread caller([&] { waited = waiting.call("wait"); });

  PipeClient other = connect();
  const Result<Json> echoed = other.call("echo", {{"text", "meanwhile"}});
  release().SetEvent();
  caller.join();
  ASSERT_TRUE(echoed.has_value()) << echoed.error().message;
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(waited->get<bool>());
}

TEST_F(PipeTest, ACallTimesOutWhenTheServerDoesNotAnswer) {
  start();
  PipeClient client = connect(50ms);
  const Result<Json> waited = client.call("wait");
  ASSERT_FALSE(waited.has_value());
  EXPECT_EQ(waited.error().code, Errc::io_error);
  EXPECT_NE(waited.error().message.find("timed out"), std::string::npos);
}

TEST(PipeClientTest, NoServerIsNotFound) {
  const Result<PipeClient> client = PipeClient::connect(unique_pipe_name(), 100ms);
  ASSERT_FALSE(client.has_value());
  EXPECT_EQ(client.error().code, Errc::not_found);
}

}  // namespace
}  // namespace myclean::platform
