#include "core/ipc/json_rpc.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/frame.hpp"
#include "core/model/result.hpp"

namespace myclean::ipc {

namespace {

using Json = nlohmann::json;

constexpr std::size_t max_token_size = 128;

bool is_ascii_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Never throws: text that is not valid UTF-8 is replaced instead of failing the dump.
std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string error_response(const Json& id, int code, const std::string& message,
                           Json data = nullptr) {
  Json error{{"code", code}, {"message", message}};
  if (!data.is_null()) {
    error["data"] = std::move(data);
  }
  return dump(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}});
}

std::string_view type_name(ParamType type) {
  switch (type) {
    case ParamType::integer:
      return "an integer";
    case ParamType::number:
      return "a number";
    case ParamType::boolean:
      return "a boolean";
    case ParamType::token:
      return "a token (letters, digits, '_', '.', '-'; no path or command line)";
    case ParamType::record:
      return "an object";
  }
  return "unknown";
}

bool has_type(const Json& value, ParamType type) {
  switch (type) {
    case ParamType::integer:
      return value.is_number_integer();
    case ParamType::number:
      return value.is_number();
    case ParamType::boolean:
      return value.is_boolean();
    case ParamType::token:
      return value.is_string() && is_token(value.get_ref<const std::string&>());
    case ParamType::record:
      return value.is_object();
  }
  return false;
}

bool printable(const std::string& text) {
  constexpr unsigned char first_printable = 0x20;
  constexpr unsigned char del = 0x7F;
  return std::ranges::none_of(text, [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte < first_printable || byte == del;
  });
}

// NOLINTNEXTLINE(misc-no-recursion): the depth is bounded by max_record_depth
std::string check_bounds(const Json& value, int depth) {
  if (depth > max_record_depth) {
    return "nested deeper than " + std::to_string(max_record_depth) + " levels";
  }
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > max_record_string || !printable(text)) {
      return "a string is not printable or longer than " + std::to_string(max_record_string) +
             " bytes";
    }
  } else if (value.is_array()) {
    if (value.size() > max_record_elements) {
      return "an array has more than " + std::to_string(max_record_elements) + " elements";
    }
    for (const Json& element : value) {
      if (std::string problem = check_bounds(element, depth + 1); !problem.empty()) {
        return problem;
      }
    }
  } else if (value.is_object()) {
    if (value.size() > max_record_members) {
      return "an object has more than " + std::to_string(max_record_members) + " members";
    }
    for (const auto& [key, member] : value.items()) {
      if (key.size() > max_record_string || !printable(key)) {
        return "a member name is not printable or longer than " +
               std::to_string(max_record_string) + " bytes";
      }
      if (std::string problem = check_bounds(member, depth + 1); !problem.empty()) {
        return problem;
      }
    }
  }
  return {};
}

// The reason `params` does not fit `declared`, or an empty string when it fits.
std::string check_params(const Json& params, const std::vector<Param>& declared) {
  for (const auto& [name, value] : params.items()) {
    const auto param = std::ranges::find(declared, name, &Param::name);
    if (param == declared.end()) {
      return "unknown parameter '" + name + "'";
    }
    if (!has_type(value, param->type)) {
      return "parameter '" + name + "' must be " + std::string(type_name(param->type));
    }
    if (param->type == ParamType::record) {
      std::string problem = check_record_bounds(value);
      if (problem.empty()) {
        problem = param->schema(value);
      }
      if (!problem.empty()) {
        return "parameter '" + name + "': " += problem;
      }
    }
  }
  for (const Param& param : declared) {
    if (param.required && !params.contains(param.name)) {
      return "missing required parameter '" + param.name + "'";
    }
  }
  return {};
}

Errc errc_named(const Json& error) {
  const auto data = error.find("data");
  if (data != error.end() && data->is_object()) {
    const auto name = data->find("errc");
    if (name != data->end() && name->is_string()) {
      // io_error is the last enumerator of Errc.
      for (int value = 0; value <= static_cast<int>(Errc::io_error); ++value) {
        const auto code = static_cast<Errc>(value);
        if (to_string(code) == name->get_ref<const std::string&>()) {
          return code;
        }
      }
    }
  }
  return Errc::io_error;
}

Errc errc_of(int code, const Json& error) {
  switch (code) {
    case rpc_error::parse_error:
    case rpc_error::invalid_request:
    case rpc_error::method_not_found:
    case rpc_error::invalid_params:
      return Errc::invalid_argument;
    case rpc_error::server_error:
      return errc_named(error);
    default:
      return Errc::io_error;
  }
}

}  // namespace

std::string check_record_bounds(const Json& value) {
  return check_bounds(value, 1);
}

bool is_token(std::string_view text) {
  if (text.empty() || text.size() > max_token_size || !is_ascii_alnum(text.front())) {
    return false;
  }
  return std::ranges::all_of(
      text, [](char c) { return is_ascii_alnum(c) || c == '_' || c == '.' || c == '-'; });
}

Result<void> Dispatcher::add(std::string method, std::vector<Param> params, Handler handler,
                             Access access) {
  return add_for_caller(
      std::move(method), std::move(params),
      [handler = std::move(handler)](const Json& request, const Caller& /*caller*/) {
        return handler(request);
      },
      access);
}

Result<void> Dispatcher::add_for_caller(std::string method, std::vector<Param> params,
                                        CallerHandler handler, Access access) {
  if (!is_token(method)) {
    return fail(Errc::invalid_argument, "method name is not a token: " + method);
  }
  for (auto param = params.begin(); param != params.end(); ++param) {
    if (!is_token(param->name)) {
      return fail(Errc::invalid_argument,
                  method + ": parameter name is not a token: " + param->name);
    }
    if (std::find_if(params.begin(), param,
                     [&](const Param& earlier) { return earlier.name == param->name; }) != param) {
      return fail(Errc::invalid_argument, method + ": parameter declared twice: " + param->name);
    }
    if (param->type == ParamType::record && !param->schema) {
      return fail(Errc::invalid_argument,
                  method + ": record parameter without a schema: " + param->name);
    }
  }
  if (methods_.contains(method)) {
    return fail(Errc::already_exists, "method already registered: " + method);
  }
  methods_.emplace(
      std::move(method),
      Method{.params = std::move(params), .handler = std::move(handler), .access = access});
  return {};
}

std::string Dispatcher::handle(std::string_view request_text, const Caller& caller) const {
  const Json request = Json::parse(request_text, nullptr, false);
  if (request.is_discarded()) {
    return error_response(nullptr, rpc_error::parse_error, "parse error: the request is not JSON");
  }
  if (request.is_array()) {
    return error_response(nullptr, rpc_error::invalid_request, "batch requests are not supported");
  }
  if (!request.is_object()) {
    return error_response(nullptr, rpc_error::invalid_request, "the request is not an object");
  }
  const auto id = request.find("id");
  if (id == request.end() || !(id->is_string() || id->is_number_integer())) {
    return error_response(nullptr, rpc_error::invalid_request,
                          "the request needs an id, a string or an integer (notifications are "
                          "not supported)");
  }
  for (const auto& [member, value] : request.items()) {
    if (member != "jsonrpc" && member != "id" && member != "method" && member != "params") {
      return error_response(*id, rpc_error::invalid_request,
                            "unknown request member '" + member + "'");
    }
  }
  const auto version = request.find("jsonrpc");
  if (version == request.end() || *version != "2.0") {
    return error_response(*id, rpc_error::invalid_request, R"(jsonrpc must be "2.0")");
  }
  const auto name = request.find("method");
  if (name == request.end() || !name->is_string()) {
    return error_response(*id, rpc_error::invalid_request, "method must be a string");
  }
  const auto method = methods_.find(name->get_ref<const std::string&>());
  if (method == methods_.end()) {
    return error_response(*id, rpc_error::method_not_found,
                          "method not found: " + name->get_ref<const std::string&>());
  }

  if (method->second.access == Access::tray && !caller.tray) {
    return error_response(
        *id, rpc_error::server_error,
        method->first + " is accepted only from the MyClean tray in the owner's session",
        Json{{"errc", to_string(Errc::refused)}});
  }

  Json params = Json::object();
  if (const auto given = request.find("params"); given != request.end()) {
    if (!given->is_object()) {
      return error_response(*id, rpc_error::invalid_params,
                            "params must be an object of named parameters");
    }
    params = *given;
  }
  if (const std::string problem = check_params(params, method->second.params); !problem.empty()) {
    return error_response(*id, rpc_error::invalid_params, method->first + ": " + problem);
  }

  try {
    Result<Json> result = method->second.handler(params, caller);
    if (!result) {
      return error_response(*id, rpc_error::server_error, result.error().message,
                            Json{{"errc", to_string(result.error().code)}});
    }
    std::string response =
        dump(Json{{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(*result)}});
    if (response.size() > max_payload_size) {
      return error_response(*id, rpc_error::internal_error,
                            method->first + ": the result is larger than " +
                                std::to_string(max_payload_size) + " bytes");
    }
    return response;
  } catch (const std::exception& error) {
    return error_response(*id, rpc_error::internal_error, method->first + ": " + error.what());
  }
}

std::string Dispatcher::handle_frame(std::string_view frame, const Caller& caller) const {
  const Result<std::string> request = decode_frame(frame);
  const std::string response = request ? handle(*request, caller)
                                       : error_response(nullptr, rpc_error::parse_error,
                                                        "parse error: " + request.error().message);
  // handle() never answers with more than max_payload_size bytes, so this cannot fail.
  Result<std::string> encoded = encode_frame(response);
  return encoded ? std::move(*encoded) : std::string{};
}

std::string make_request(std::int64_t id, std::string_view method, const Json& params) {
  return dump(
      Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", params}});
}

Result<Json> parse_response(std::string_view response_text, std::int64_t id) {
  const Json response = Json::parse(response_text, nullptr, false);
  if (response.is_discarded() || !response.is_object()) {
    return fail(Errc::invalid_data, "the response is not a JSON object");
  }
  if (const auto version = response.find("jsonrpc");
      version == response.end() || *version != "2.0") {
    return fail(Errc::invalid_data, "the response is not JSON-RPC 2.0");
  }
  const auto answered = response.find("id");
  const bool same_id = answered != response.end() && answered->is_number_integer() &&
                       answered->get<std::int64_t>() == id;
  // An error that the server could not attribute to a request carries a null id.
  const bool unattributed = answered != response.end() && answered->is_null();

  if (const auto error = response.find("error"); error != response.end()) {
    const auto code = error->is_object() ? error->find("code") : error->end();
    const auto message = error->is_object() ? error->find("message") : error->end();
    if (!(same_id || unattributed) || code == error->end() || !code->is_number_integer() ||
        message == error->end() || !message->is_string()) {
      return fail(Errc::invalid_data, "malformed JSON-RPC error response");
    }
    const int value = code->get<int>();
    return fail(errc_of(value, *error), "json-rpc error " + std::to_string(value) + ": " +
                                            message->get_ref<const std::string&>());
  }
  if (!same_id) {
    return fail(Errc::invalid_data, "the response does not answer request " + std::to_string(id));
  }
  const auto result = response.find("result");
  if (result == response.end()) {
    return fail(Errc::invalid_data, "the response has neither result nor error");
  }
  return *result;
}

}  // namespace myclean::ipc
