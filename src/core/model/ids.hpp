#pragma once

#include <compare>
#include <cstdint>

namespace myclean {

// A strongly typed database id. The value 0 means "not stored yet"; stored rows start at 1.
// Modules define their own ids the same way: `using RunId = Id<struct RunTag>;`.
template <typename Tag>
class Id {
 public:
  constexpr Id() = default;
  constexpr explicit Id(std::int64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::int64_t value() const { return value_; }

  friend constexpr auto operator<=>(const Id&, const Id&) = default;

 private:
  std::int64_t value_ = 0;
};

using FindingId = Id<struct FindingTag>;
using PlanId = Id<struct PlanTag>;
using PlanItemId = Id<struct PlanItemTag>;

}  // namespace myclean
