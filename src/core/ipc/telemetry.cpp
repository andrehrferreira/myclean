#include "core/ipc/telemetry.hpp"

#include <string>
#include <string_view>

namespace myclean::ipc {

std::string entity_token(std::string_view entity) {
  std::string token;
  for (const char c : entity) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (allowed) {
      token.push_back(c);
    } else if (!token.empty() && token.back() != '_') {
      token.push_back('_');
    }
  }
  if (!token.empty() && token.back() == '_') {
    token.pop_back();
  }
  return token;
}

}  // namespace myclean::ipc
