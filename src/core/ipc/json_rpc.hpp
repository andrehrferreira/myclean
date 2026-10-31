#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"

namespace myclean::ipc {

// JSON-RPC 2.0 error codes used by the service.
namespace rpc_error {
inline constexpr int parse_error = -32700;
inline constexpr int invalid_request = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params = -32602;
inline constexpr int internal_error = -32603;
// A method ran and failed; error.data.errc holds the Errc name.
inline constexpr int server_error = -32000;
}  // namespace rpc_error

// True for a name or value made only of ASCII letters, digits and '_', '.', '-', starting with a
// letter or digit, at most 128 characters. No path separator, drive colon, space, quote, '%' or
// shell metacharacter fits, so no file path and no command line can be passed as a token.
[[nodiscard]] bool is_token(std::string_view text);

// The parameter types a method may declare. Strings are always tokens; there is no free-text,
// path, array or object parameter.
enum class ParamType {
  integer,  // a JSON integer
  number,   // any JSON number
  boolean,
  token,  // a string that is_token() accepts
};

struct Param {
  std::string name;
  ParamType type = ParamType::token;
  bool required = false;
};

// A method implementation. `params` is an object holding only declared parameters of the
// declared types. Called concurrently from several client connections.
using Handler = std::function<Result<nlohmann::json>(const nlohmann::json& params)>;

// The server side of the protocol: typed methods, registered by name.
//
// A request must be a JSON object {"jsonrpc": "2.0", "id": <string or integer>, "method": ...,
// "params": {...}} with no other member. Parameters are passed by name; positional parameters,
// batches and notifications (requests without an id) are refused. Answers:
//   -32700  the text is not JSON (or the frame is malformed)
//   -32600  not a valid request object
//   -32601  the method is not registered (nothing runs)
//   -32602  an undeclared parameter, a missing required one, or a value of the wrong type,
//           including any string that is not a token (nothing runs)
//   -32603  the handler threw
//   -32000  the handler returned an error; error.data.errc names its Errc
//
// Register every method before the first request; add() is not synchronized with handle().
class Dispatcher {
 public:
  // Fails with already_exists for a registered method, invalid_argument when the method or a
  // parameter name is not a token or a parameter is declared twice.
  [[nodiscard]] Result<void> add(std::string method, std::vector<Param> params, Handler handler);

  // The JSON-RPC 2.0 response to one request text.
  [[nodiscard]] std::string handle(std::string_view request_text) const;
  // The same for a frame (see frame.hpp): decodes, handles and encodes the response frame.
  [[nodiscard]] std::string handle_frame(std::string_view frame) const;

 private:
  struct Method {
    std::vector<Param> params;
    Handler handler;
  };

  std::map<std::string, Method, std::less<>> methods_;
};

// Client side: the text of request `id` calling `method` with named `params` (an object).
[[nodiscard]] std::string make_request(std::int64_t id, std::string_view method,
                                       const nlohmann::json& params);
// The result member of the response to request `id`. A JSON-RPC error becomes an Error whose
// message starts with "json-rpc error <code>: "; its Errc is invalid_argument for -32700,
// -32600, -32601 and -32602, the server's Errc for -32000, and io_error otherwise. A malformed
// response or one for another id fails with invalid_data.
[[nodiscard]] Result<nlohmann::json> parse_response(std::string_view response_text,
                                                    std::int64_t id);

}  // namespace myclean::ipc
