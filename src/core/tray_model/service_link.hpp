#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"

namespace myclean::tray_model {

// The tray's connection to the service. Implemented over the pipe in src/tray; tests use a fake.
class IServiceLink {
 public:
  virtual ~IServiceLink() = default;
  IServiceLink(const IServiceLink&) = delete;
  IServiceLink& operator=(const IServiceLink&) = delete;
  IServiceLink(IServiceLink&&) = delete;
  IServiceLink& operator=(IServiceLink&&) = delete;

  // The result of `method`, or the JSON-RPC error the service answered, or a failure to reach
  // the service at all (then reachable() is false until a later call gets through).
  [[nodiscard]] virtual Result<nlohmann::json> call(std::string_view method,
                                                    const nlohmann::json& params) = 0;
  // Whether the last call got an answer from the service, error answers included.
  [[nodiscard]] virtual bool reachable() const = 0;

 protected:
  IServiceLink() = default;
};

}  // namespace myclean::tray_model
