#include "core/drives/history.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean::drives {

namespace {

constexpr std::array<std::string_view, 3> kind_names = {"retrim", "defrag", "other"};
constexpr std::uint32_t code_retrim = 0x2BC;
constexpr std::uint32_t code_defrag = 0x1ED;
constexpr std::size_t code_offset = 4;

std::uint32_t little_endian(std::span<const std::uint8_t> data, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[offset + index]) << (8U * index);
  }
  return value;
}

std::string lower(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  });
  return result;
}

OptimizationKind kind_of(const OptimizerEvent& event) {
  if (event.data.size() >= code_offset + 4) {
    const std::uint32_t code = little_endian(event.data, code_offset);
    if (code == code_retrim) {
      return OptimizationKind::retrim;
    }
    if (code == code_defrag) {
      return OptimizationKind::defrag;
    }
  }
  const std::string operation = lower(event.operation);
  if (operation.find("retrim") != std::string::npos) {
    return OptimizationKind::retrim;
  }
  return operation.find("defrag") != std::string::npos ? OptimizationKind::defrag
                                                       : OptimizationKind::other;
}

// The drive letter of "Label (K:)"; 0 when the text does not end that way.
char drive_letter(std::string_view volume) {
  if (volume.size() < 4 || !volume.ends_with(":)") || volume[volume.size() - 4] != '(') {
    return 0;
  }
  const auto letter = static_cast<unsigned char>(volume[volume.size() - 3]);
  return std::isalpha(letter) != 0 ? static_cast<char>(std::toupper(letter)) : char{0};
}

void keep_newest(std::optional<WallTime>& newest, WallTime time) {
  if (!newest || time > *newest) {
    newest = time;
  }
}

}  // namespace

std::string_view to_string(OptimizationKind kind) {
  return detail::enum_name(kind_names, kind);
}

std::optional<Optimization> interpret(const OptimizerEvent& event) {
  if (event.id != event_optimized && event.id != event_failed) {
    return std::nullopt;
  }
  const char drive = drive_letter(event.volume);
  if (drive == 0) {
    return std::nullopt;
  }
  const bool result_ok = event.data.size() < 4 || little_endian(event.data, 0) == 0;
  return Optimization{.time = event.time,
                      .drive = drive,
                      .kind = kind_of(event),
                      .succeeded = event.id == event_optimized && result_ok,
                      .operation = event.operation};
}

LastRuns last_runs(std::span<const Optimization> history, char drive) {
  LastRuns runs;
  for (const Optimization& run : history) {
    if (!runs.log_since || run.time < *runs.log_since) {
      runs.log_since = run.time;
    }
    if (run.drive != drive || !run.succeeded) {
      continue;
    }
    if (run.kind == OptimizationKind::retrim) {
      keep_newest(runs.retrim, run.time);
    } else if (run.kind == OptimizationKind::defrag) {
      keep_newest(runs.defrag, run.time);
    }
  }
  return runs;
}

}  // namespace myclean::drives
