#pragma once

#include <chrono>
#include <compare>
#include <cstdint>

namespace myclean {

// A count of bytes. Durations are std::chrono durations; points in time are WallTime and
// MonoTime below.
class Bytes {
 public:
  constexpr Bytes() = default;
  constexpr explicit Bytes(std::uint64_t count) : count_(count) {}

  [[nodiscard]] constexpr std::uint64_t count() const { return count_; }

  constexpr Bytes& operator+=(Bytes other) {
    count_ += other.count_;
    return *this;
  }
  friend constexpr Bytes operator+(Bytes left, Bytes right) { return left += right; }
  friend constexpr auto operator<=>(const Bytes&, const Bytes&) = default;

 private:
  std::uint64_t count_ = 0;
};

namespace literals {

constexpr Bytes operator""_B(unsigned long long count) {
  return Bytes{count};
}
constexpr Bytes operator""_KiB(unsigned long long count) {
  return Bytes{count << 10U};
}
constexpr Bytes operator""_MiB(unsigned long long count) {
  return Bytes{count << 20U};
}
constexpr Bytes operator""_GiB(unsigned long long count) {
  return Bytes{count << 30U};
}

}  // namespace literals

// Wall-clock time (UTC); follows manual clock changes. Use for timestamps that are stored.
using WallTime = std::chrono::system_clock::time_point;
// Monotonic time; never jumps. Use for intervals and timeouts.
using MonoTime = std::chrono::steady_clock::time_point;

}  // namespace myclean
