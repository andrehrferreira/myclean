#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace myclean::detail {

// Text form of an enumerator whose values are 0..N-1, in declaration order.
template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::string_view enum_name(const std::array<std::string_view, N>& names,
                                                   Enum value) {
  return names.at(static_cast<std::size_t>(value));
}

template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::optional<Enum> enum_from_name(
    const std::array<std::string_view, N>& names, std::string_view text) {
  for (std::size_t index = 0; index < N; ++index) {
    if (names.at(index) == text) {
      return static_cast<Enum>(index);
    }
  }
  return std::nullopt;
}

}  // namespace myclean::detail
