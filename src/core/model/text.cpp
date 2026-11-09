#include "core/model/text.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace myclean {
namespace {

void append(std::string& out, char32_t code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0U | (code >> 6U));
    out += static_cast<char>(0x80U | (code & 0x3FU));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0U | (code >> 12U));
    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (code & 0x3FU));
  } else {
    out += static_cast<char>(0xF0U | (code >> 18U));
    out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (code & 0x3FU));
  }
}

constexpr char32_t replacement = 0xFFFD;

bool high_surrogate(char32_t unit) {
  return unit >= 0xD800 && unit <= 0xDBFF;
}
bool low_surrogate(char32_t unit) {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

}  // namespace

std::string utf8_text(const std::filesystem::path& path) {
  const std::filesystem::path::string_type& native = path.native();
  std::string out;
  out.reserve(native.size());
  static_assert(sizeof(std::filesystem::path::value_type) == 2, "Windows paths are UTF-16");
  for (std::size_t index = 0; index < native.size(); ++index) {
    const auto unit = static_cast<char32_t>(static_cast<std::uint16_t>(native[index]));
    if (high_surrogate(unit) && index + 1 < native.size()) {
      const auto next = static_cast<char32_t>(static_cast<std::uint16_t>(native[index + 1]));
      if (low_surrogate(next)) {
        append(out, 0x10000 + ((unit - 0xD800) << 10U) + (next - 0xDC00));
        ++index;
        continue;
      }
    }
    append(out, high_surrogate(unit) || low_surrogate(unit) ? replacement : unit);
  }
  return out;
}

}  // namespace myclean
