#pragma once

#include <cstddef>
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

// The parameter types a method may declare. Strings are tokens, with one exception: the strings
// inside a `record`. There is no free-text, path or array parameter.
enum class ParamType {
  integer,  // a JSON integer
  number,   // any JSON number
  boolean,
  token,  // a string that is_token() accepts
  // A JSON object of data to store, such as an input incident the tray measured. It is the only
  // way a string that is not a token reaches the service, so it is held to two checks, both
  // before the handler runs: the generic bounds of within_record_bounds(), and the method's own
  // strict schema (Param::schema: every member known and typed, unknown members rejected). A
  // record is stored and displayed as data only; its strings are NEVER passed to a file,
  // process, registry or any other OS API (architecture section 8).
  record,
};

// Generic bounds of a record (ParamType::record): nesting depth, members per object, elements
// per array, and the size of every string, key or value. Strings must also be printable: no
// control character (U+0000-U+001F, U+007F). The parser already rejects invalid UTF-8.
inline constexpr int max_record_depth = 4;
inline constexpr std::size_t max_record_members = 32;
inline constexpr std::size_t max_record_elements = 64;
inline constexpr std::size_t max_record_string = 260;

// The reason `value` exceeds the bounds above, or an empty string when it fits.
[[nodiscard]] std::string check_record_bounds(const nlohmann::json& value);

// A method-specific record schema: the reason `record` does not fit, or an empty string.
using RecordSchema = std::function<std::string(const nlohmann::json& record)>;

struct Param {
  std::string name;
  ParamType type = ParamType::token;
  bool required = false;
  RecordSchema schema;  // ParamType::record only, and required there
};

// A method implementation. `params` is an object holding only declared parameters of the
// declared types. Called concurrently from several client connections.
using Handler = std::function<Result<nlohmann::json>(const nlohmann::json& params)>;

// Who may call a method.
enum class Access {
  any,   // every client the pipe admits
  tray,  // only the owner's MyClean tray (Caller::tray); approvals and user tasks
};

// Who sent a request, as the transport established it. The default is an unverified caller.
struct Caller {
  // The client process is the configured myclean-tray.exe running as the owner in an
  // interactive session (checked by the service from the pipe client's process id).
  bool tray = false;
};

// A method implementation that also gets its caller.
using CallerHandler =
    std::function<Result<nlohmann::json>(const nlohmann::json& params, const Caller& caller)>;

// The server side of the protocol: typed methods, registered by name.
//
// A request must be a JSON object {"jsonrpc": "2.0", "id": <string or integer>, "method": ...,
// "params": {...}} with no other member. Parameters are passed by name; positional parameters,
// batches and notifications (requests without an id) are refused. Answers:
//   -32700  the text is not JSON (or the frame is malformed)
//   -32600  not a valid request object
//   -32601  the method is not registered (nothing runs)
//   -32602  an undeclared parameter, a missing required one, or a value of the wrong type,
//           including any string that is not a token and any record that exceeds the bounds or
//           does not fit its method's schema (nothing runs)
//   -32603  the handler threw
//   -32000  the handler returned an error; error.data.errc names its Errc; also a method of
//           Access::tray called by any other caller (errc "refused", nothing runs)
//
// Register every method before the first request; add() is not synchronized with handle().
class Dispatcher {
 public:
  // Fails with already_exists for a registered method, invalid_argument when the method or a
  // parameter name is not a token, a parameter is declared twice, or a record has no schema.
  [[nodiscard]] Result<void> add(std::string method, std::vector<Param> params, Handler handler,
                                 Access access = Access::any);
  // The same for a handler that needs to know its caller (a proposal of the tray is the owner's).
  [[nodiscard]] Result<void> add_for_caller(std::string method, std::vector<Param> params,
                                            CallerHandler handler, Access access = Access::any);

  // The JSON-RPC 2.0 response to one request text from `caller`.
  [[nodiscard]] std::string handle(std::string_view request_text, const Caller& caller = {}) const;
  // The same for a frame (see frame.hpp): decodes, handles and encodes the response frame.
  [[nodiscard]] std::string handle_frame(std::string_view frame, const Caller& caller = {}) const;

 private:
  struct Method {
    std::vector<Param> params;
    CallerHandler handler;
    Access access = Access::any;
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
