#pragma once

#include <string>
#include <string_view>

#include "core/model/result.hpp"

namespace myclean {

// Who requested a plan transition. Recorded with every action_log entry.
class Actor {
 public:
  enum class Kind { scheduler, governor, owner, mcp };

  [[nodiscard]] static Actor scheduler();
  [[nodiscard]] static Actor governor();
  [[nodiscard]] static Actor owner();
  // An MCP client such as "claude-code". The name must be non-empty and free of control
  // characters, because it is written to the audit log.
  [[nodiscard]] static Result<Actor> mcp(std::string client);
  // Parses the text form written by to_string(): "scheduler", "governor", "owner" or
  // "mcp:<client>".
  [[nodiscard]] static Result<Actor> parse(std::string_view text);

  [[nodiscard]] Kind kind() const { return kind_; }
  // The MCP client name; empty unless kind() is Kind::mcp.
  [[nodiscard]] const std::string& client() const { return client_; }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Actor&, const Actor&) = default;

 private:
  Actor(Kind kind, std::string client);

  Kind kind_;
  std::string client_;
};

}  // namespace myclean
