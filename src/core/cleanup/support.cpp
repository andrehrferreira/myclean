#include "core/cleanup/support.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/units.hpp"

namespace myclean::cleanup {

std::string_view trim(std::string_view text) {
  constexpr std::string_view blank = " \t\r\n";
  const std::size_t first = text.find_first_not_of(blank);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(blank) - first + 1);
}

std::vector<std::string_view> lines(std::string_view text) {
  std::vector<std::string_view> found;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::string_view line = trim(text.substr(0, end));
    if (!line.empty()) {
      found.push_back(line);
    }
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  return found;
}

std::vector<std::string> words(std::string_view text) {
  std::vector<std::string> found;
  std::size_t start = text.find_first_not_of(' ');
  while (start != std::string_view::npos) {
    const std::size_t end = text.find(' ', start);
    found.emplace_back(text.substr(start, end - start));
    start = end == std::string_view::npos ? end : text.find_first_not_of(' ', end);
  }
  return found;
}

std::filesystem::path from_utf8(std::string_view text) {
  return {std::u8string(text.begin(), text.end())};
}

std::optional<Bytes> parse_size(std::string_view text, std::uint64_t base) {
  text = trim(text);
  double number = 0.0;
  const char* const first = std::to_address(text.begin());
  const auto [rest, error] = std::from_chars(first, std::to_address(text.end()), number);
  if (error != std::errc{} || number < 0.0) {
    return std::nullopt;
  }
  std::string unit(trim(text.substr(static_cast<std::size_t>(std::distance(first, rest)))));
  std::ranges::transform(unit, unit.begin(), [](char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  });
  constexpr std::array<std::string_view, 5> multiples = {"kb", "mb", "gb", "tb", "pb"};
  double factor = 1.0;
  if (unit != "b" && unit != "byte" && unit != "bytes") {
    const auto found = std::ranges::find(multiples, unit);
    if (found == multiples.end()) {
      return std::nullopt;
    }
    for (auto power = multiples.begin(); power <= found; ++power) {
      factor *= static_cast<double>(base);
    }
  }
  return Bytes{static_cast<std::uint64_t>(std::llround(number * factor))};
}

Verification compare(std::string metric, Bytes before, Bytes after) {
  Effect effect = Effect::unchanged;
  if (after < before) {
    effect = Effect::better;
  } else if (after > before) {
    effect = Effect::worse;
  }
  return Verification{.metric = std::move(metric),
                      .before = static_cast<double>(before.count()),
                      .after = static_cast<double>(after.count()),
                      .effect = effect};
}

}  // namespace myclean::cleanup
