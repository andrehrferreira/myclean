#include "core/ipc/json_rpc.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/ipc/frame.hpp"
#include "core/model/result.hpp"

namespace myclean::ipc {
namespace {

using Json = nlohmann::json;

class DispatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dispatcher_
                    .add("volume.get",
                         {Param{.name = "volume", .type = ParamType::token, .required = true},
                          Param{.name = "days", .type = ParamType::integer},
                          Param{.name = "ratio", .type = ParamType::number},
                          Param{.name = "verbose", .type = ParamType::boolean}},
                         [this](const Json& params) -> Result<Json> {
                           ++calls_;
                           return Json{{"echo", params}};
                         })
                    .has_value());
    ASSERT_TRUE(dispatcher_
                    .add("fails", {},
                         [](const Json&) -> Result<Json> {
                           return fail(Errc::not_found, "no such volume");
                         })
                    .has_value());
    ASSERT_TRUE(dispatcher_
                    .add("throws", {},
                         [](const Json&) -> Result<Json> { throw std::runtime_error("boom"); })
                    .has_value());
  }

  // The parsed response to `request`.
  [[nodiscard]] Json answer(std::string_view request) const {
    return Json::parse(dispatcher_.handle(request));
  }

  // The parsed response to a well-formed request of `method` with `params`.
  [[nodiscard]] Json call(std::string_view method, const Json& params) const {
    return answer(make_request(7, method, params));
  }

  [[nodiscard]] Dispatcher& dispatcher() { return dispatcher_; }
  [[nodiscard]] int calls() const { return calls_; }

 private:
  Dispatcher dispatcher_;
  int calls_ = 0;
};

int error_code(const Json& response) {
  return response.at("error").at("code").get<int>();
}

TEST_F(DispatcherTest, CallsATypedMethodAndEchoesTheId) {
  const Json response =
      call("volume.get", {{"volume", "C"}, {"days", 30}, {"ratio", 0.5}, {"verbose", true}});
  EXPECT_EQ(response.at("jsonrpc"), "2.0");
  EXPECT_EQ(response.at("id"), 7);
  EXPECT_EQ(response.at("result").at("echo").at("days"), 30);
  EXPECT_EQ(calls(), 1);

  const Json by_string_id = answer(
      R"({"jsonrpc":"2.0","id":"a1","method":"volume.get","params":{"volume":"notepad.exe"}})");
  EXPECT_EQ(by_string_id.at("id"), "a1");
  EXPECT_TRUE(by_string_id.contains("result"));
}

TEST_F(DispatcherTest, UnknownMethodIsMethodNotFoundAndRunsNothing) {
  const Json response = call("file.delete", {{"volume", "C"}});
  EXPECT_EQ(error_code(response), rpc_error::method_not_found);
  EXPECT_EQ(response.at("id"), 7);
  EXPECT_EQ(calls(), 0);
}

TEST_F(DispatcherTest, RawPathsAndCommandLinesAreInvalidParamsAndRunNothing) {
  // An extra parameter carrying a path, and paths or command lines in a declared token.
  const std::vector<Json> smuggled{
      Json{{"volume", "C"}, {"path", R"(C:\Windows\System32\config)"}},
      Json{{"volume", R"(C:\Windows\System32)"}},
      Json{{"volume", "C:"}},
      Json{{"volume", "/etc/passwd"}},
      Json{{"volume", R"(\\server\share)"}},
      Json{{"volume", ".."}},
      Json{{"volume", R"(..\..\secret)"}},
      Json{{"volume", "%WINDIR%"}},
      Json{{"volume", "cmd /c del *"}},
      Json{{"volume", R"(powershell.exe -c "rm -r ~")"}},
      Json{{"volume", "a|b"}},
      Json{{"volume", "a&b"}},
      Json{{"volume", ""}},
      Json{{"volume", std::string(129, 'a')}},
      Json{{"volume", Json::array({"C"})}},
      Json{{"volume", Json{{"path", R"(C:\)"}}}},
  };
  for (const Json& params : smuggled) {
    const Json response = call("volume.get", params);
    EXPECT_EQ(error_code(response), rpc_error::invalid_params) << params.dump();
  }
  EXPECT_EQ(calls(), 0);
}

TEST_F(DispatcherTest, WrongTypesAndMissingParamsAreInvalidParams) {
  EXPECT_EQ(error_code(call("volume.get", {{"volume", "C"}, {"days", "30"}})),
            rpc_error::invalid_params);
  EXPECT_EQ(error_code(call("volume.get", {{"volume", "C"}, {"days", 1.5}})),
            rpc_error::invalid_params);
  EXPECT_EQ(error_code(call("volume.get", {{"volume", "C"}, {"verbose", 1}})),
            rpc_error::invalid_params);
  EXPECT_EQ(error_code(call("volume.get", Json::object())), rpc_error::invalid_params);
  EXPECT_EQ(error_code(answer(R"({"jsonrpc":"2.0","id":1,"method":"volume.get","params":["C"]})")),
            rpc_error::invalid_params);
  EXPECT_EQ(calls(), 0);
}

TEST_F(DispatcherTest, MalformedRequestsAreRefused) {
  const Json parse_error = answer("{not json");
  EXPECT_EQ(error_code(parse_error), rpc_error::parse_error);
  EXPECT_TRUE(parse_error.at("id").is_null());

  EXPECT_EQ(error_code(answer(R"([{"jsonrpc":"2.0","id":1,"method":"fails"}])")),
            rpc_error::invalid_request);
  EXPECT_EQ(error_code(answer(R"("text")")), rpc_error::invalid_request);
  EXPECT_EQ(error_code(answer(R"({"jsonrpc":"2.0","method":"fails"})")),
            rpc_error::invalid_request);
  EXPECT_EQ(error_code(answer(R"({"jsonrpc":"2.0","id":null,"method":"fails"})")),
            rpc_error::invalid_request);
  EXPECT_EQ(error_code(answer(R"({"jsonrpc":"2.0","id":1.5,"method":"fails"})")),
            rpc_error::invalid_request);
  EXPECT_EQ(error_code(answer(R"({"jsonrpc":"1.0","id":1,"method":"fails"})")),
            rpc_error::invalid_request);
  EXPECT_EQ(error_code(answer(R"({"jsonrpc":"2.0","id":1,"method":5})")),
            rpc_error::invalid_request);
  const Json extra = answer(R"({"jsonrpc":"2.0","id":3,"method":"fails","cmd":"del C:\\"})");
  EXPECT_EQ(error_code(extra), rpc_error::invalid_request);
  EXPECT_EQ(extra.at("id"), 3);
}

TEST_F(DispatcherTest, HandlerErrorsBecomeServerAndInternalErrors) {
  const Json failed = call("fails", Json::object());
  EXPECT_EQ(error_code(failed), rpc_error::server_error);
  EXPECT_EQ(failed.at("error").at("message"), "no such volume");
  EXPECT_EQ(failed.at("error").at("data").at("errc"), "not_found");

  const Json thrown = call("throws", Json::object());
  EXPECT_EQ(error_code(thrown), rpc_error::internal_error);
}

TEST_F(DispatcherTest, RegistrationRejectsDuplicatesAndNonTokens) {
  const auto ok = [](const Json&) -> Result<Json> { return Json(nullptr); };
  const Result<void> duplicate = dispatcher().add("fails", {}, ok);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code, Errc::already_exists);
  EXPECT_FALSE(dispatcher().add("bad name", {}, ok).has_value());
  EXPECT_FALSE(dispatcher().add("m", {Param{.name = "a/b"}}, ok).has_value());
  EXPECT_FALSE(dispatcher().add("m", {Param{.name = "a"}, Param{.name = "a"}}, ok).has_value());
}

TEST_F(DispatcherTest, HandlesFrames) {
  const Result<std::string> request = encode_frame(make_request(9, "fails", Json::object()));
  ASSERT_TRUE(request.has_value());
  const Result<std::string> response = decode_frame(dispatcher().handle_frame(*request));
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(error_code(Json::parse(*response)), rpc_error::server_error);

  const Result<std::string> broken = decode_frame(dispatcher().handle_frame("\x09\x00"));
  ASSERT_TRUE(broken.has_value());
  EXPECT_EQ(error_code(Json::parse(*broken)), rpc_error::parse_error);
}

TEST(TokenTest, AcceptsNamesAndRejectsEverythingElse) {
  for (const std::string_view token :
       {"a", "C", "status", "metrics.query", "notepad.exe", "disk-0", "S_1", "0abc"}) {
    EXPECT_TRUE(is_token(token)) << token;
  }
  for (const std::string_view text : {"", ".", "..", "-rf", ".hidden", "a b", "a/b", R"(a\b)", "C:",
                                      "a:b", "%x%", "a;b", R"(a"b)", "a'b", "a$b", "\xC3\xA9"}) {
    EXPECT_FALSE(is_token(text)) << text;
  }
}

TEST(ParseResponseTest, ReturnsTheResultOfTheMatchingResponse) {
  const Result<Json> result = parse_response(R"({"jsonrpc":"2.0","id":4,"result":{"ok":true}})", 4);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->at("ok"), true);
}

TEST(ParseResponseTest, TurnsErrorsIntoResultErrors) {
  const Result<Json> unknown = parse_response(
      R"({"jsonrpc":"2.0","id":4,"error":{"code":-32601,"message":"method not found: x"}})", 4);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, Errc::invalid_argument);
  EXPECT_EQ(unknown.error().message, "json-rpc error -32601: method not found: x");

  const Result<Json> server = parse_response(
      R"({"jsonrpc":"2.0","id":4,"error":{"code":-32000,"message":"gone","data":{"errc":"not_found"}}})",
      4);
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error().code, Errc::not_found);

  const Result<Json> unattributed = parse_response(
      R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error"}})", 4);
  ASSERT_FALSE(unattributed.has_value());
  EXPECT_EQ(unattributed.error().code, Errc::invalid_argument);

  const Result<Json> internal =
      parse_response(R"({"jsonrpc":"2.0","id":4,"error":{"code":-32603,"message":"boom"}})", 4);
  ASSERT_FALSE(internal.has_value());
  EXPECT_EQ(internal.error().code, Errc::io_error);
}

TEST(ParseResponseTest, RejectsMalformedOrForeignResponses) {
  for (const std::string_view response :
       {"not json", "[]", R"({"id":4,"result":1})", R"({"jsonrpc":"2.0","id":5,"result":1})",
        R"({"jsonrpc":"2.0","id":4})", R"({"jsonrpc":"2.0","id":4,"error":{"code":"x"}})"}) {
    const Result<Json> parsed = parse_response(response, 4);
    ASSERT_FALSE(parsed.has_value()) << response;
    EXPECT_EQ(parsed.error().code, Errc::invalid_data) << response;
  }
}

}  // namespace
}  // namespace myclean::ipc
