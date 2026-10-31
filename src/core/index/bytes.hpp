#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

// Little-endian field access in on-disk structures (the index runs on x64 only).
namespace myclean::index {

// True when `size` bytes starting at `offset` lie inside `bytes`.
[[nodiscard]] constexpr bool fits(std::span<const std::byte> bytes, std::size_t offset,
                                  std::size_t size) {
  return offset <= bytes.size() && size <= bytes.size() - offset;
}

// The value at `offset`; the caller has checked fits(bytes, offset, sizeof(T)).
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] T load(std::span<const std::byte> bytes, std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.subspan(offset, sizeof(T)).data(), sizeof(T));
  return value;
}

template <typename T>
  requires std::is_trivially_copyable_v<T>
void store(std::span<std::byte> bytes, std::size_t offset, T value) {
  std::memcpy(bytes.subspan(offset, sizeof(T)).data(), &value, sizeof(T));
}

}  // namespace myclean::index
