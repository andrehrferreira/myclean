#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/model/ids.hpp"
#include "core/model/units.hpp"

namespace myclean {

enum class Severity { info, warning, critical };

[[nodiscard]] std::string_view to_string(Severity severity);
[[nodiscard]] std::optional<Severity> parse_severity(std::string_view text);

// Something an analyzer detected, such as a volume filling up or cleanable items.
struct Finding {
  FindingId id;
  std::string kind;     // detector-defined, dotted: "volume.fill_up", "clean.cache"
  std::string subject;  // what it is about: a volume, disk, process image or path
  Severity severity = Severity::info;
  std::string summary;  // one line for the owner
  WallTime detected_at;
};

}  // namespace myclean
