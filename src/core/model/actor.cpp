#include "core/model/actor.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "core/model/result.hpp"

namespace myclean {

namespace {

constexpr std::string_view mcp_prefix = "mcp:";

bool has_control_character(std::string_view text) {
  return std::ranges::any_of(text, [](char character) {
    const auto code = static_cast<unsigned char>(character);
    return code < 0x20U || code == 0x7FU;
  });
}

}  // namespace

Actor::Actor(Kind kind, std::string client) : kind_(kind), client_(std::move(client)) {}

Actor Actor::scheduler() {
  return {Kind::scheduler, {}};
}

Actor Actor::governor() {
  return {Kind::governor, {}};
}

Actor Actor::owner() {
  return {Kind::owner, {}};
}

Result<Actor> Actor::mcp(std::string client) {
  if (client.empty()) {
    return fail(Errc::invalid_argument, "MCP actor needs a client name");
  }
  if (has_control_character(client)) {
    return fail(Errc::invalid_argument, "MCP client name contains a control character");
  }
  return Actor{Kind::mcp, std::move(client)};
}

Result<Actor> Actor::parse(std::string_view text) {
  if (text == "scheduler") {
    return scheduler();
  }
  if (text == "governor") {
    return governor();
  }
  if (text == "owner") {
    return owner();
  }
  if (text.starts_with(mcp_prefix)) {
    return mcp(std::string{text.substr(mcp_prefix.size())});
  }
  return fail(Errc::invalid_argument, "unknown actor '" + std::string{text} + "'");
}

std::string Actor::to_string() const {
  switch (kind_) {
    case Kind::scheduler:
      return "scheduler";
    case Kind::governor:
      return "governor";
    case Kind::owner:
      return "owner";
    case Kind::mcp:
      return std::string{mcp_prefix} + client_;
  }
  return {};
}

}  // namespace myclean
