#include "core/input/incident.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/enum_names.hpp"

namespace myclean::input {

namespace {

constexpr std::array<std::string_view, 2> chord_names = {"copy", "paste"};

// CF_TEXT (1) .. CF_DIBV5 (17), indexed by id - 1.
constexpr std::array<std::string_view, 17> standard_format_names = {
    "CF_TEXT",        "CF_BITMAP",      "CF_METAFILEPICT", "CF_SYLK",    "CF_DIF",  "CF_TIFF",
    "CF_OEMTEXT",     "CF_DIB",         "CF_PALETTE",      "CF_PENDATA", "CF_RIFF", "CF_WAVE",
    "CF_UNICODETEXT", "CF_ENHMETAFILE", "CF_HDROP",        "CF_LOCALE",  "CF_DIBV5"};

enum class Group { none, text, bitmap, metafile };

Group group_of(std::uint32_t id) {
  switch (id) {
    case 1:   // CF_TEXT
    case 7:   // CF_OEMTEXT
    case 13:  // CF_UNICODETEXT
      return Group::text;
    case 2:   // CF_BITMAP
    case 8:   // CF_DIB
    case 17:  // CF_DIBV5
      return Group::bitmap;
    case 3:   // CF_METAFILEPICT
    case 14:  // CF_ENHMETAFILE
      return Group::metafile;
    default:
      return Group::none;
  }
}

bool equal_ignoring_case(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

constexpr std::array<std::string_view, 4> suspects = {
    "PowerToys.KeyboardManagerEngine.exe", "AnyDesk.exe", "ArmouryCrate.UserSessionHelper.exe",
    "Discord.exe"};

}  // namespace

std::string_view to_string(Chord chord) {
  return detail::enum_name(chord_names, chord);
}

std::optional<Chord> parse_chord(std::string_view text) {
  return detail::enum_from_name<Chord>(chord_names, text);
}

std::vector<ClipboardFormat> describe_formats(std::vector<ClipboardFormat> formats) {
  std::vector<Group> seen;
  for (ClipboardFormat& format : formats) {
    if (format.name.empty() && format.id >= 1 && format.id <= standard_format_names.size()) {
      format.name = standard_format_names.at(format.id - 1);
    }
    const Group group = group_of(format.id);
    if (group == Group::none) {
      continue;
    }
    format.synthesized = std::ranges::find(seen, group) != seen.end();
    seen.push_back(group);
  }
  return formats;
}

bool is_heavy(const ClipboardFormat& format) {
  switch (format.id) {
    case 2:   // CF_BITMAP
    case 3:   // CF_METAFILEPICT
    case 6:   // CF_TIFF
    case 8:   // CF_DIB
    case 14:  // CF_ENHMETAFILE
    case 17:  // CF_DIBV5
      return true;
    default:
      break;
  }
  constexpr std::array<std::string_view, 5> heavy_names = {"PNG", "JFIF", "GIF", "HTML Format",
                                                           "Rich Text Format"};
  return std::ranges::any_of(
      heavy_names, [&](std::string_view name) { return equal_ignoring_case(format.name, name); });
}

std::span<const std::string_view> known_suspects() {
  return suspects;
}

std::vector<std::string> running_suspects(std::span<const std::string> running) {
  std::vector<std::string> found;
  for (const std::string_view suspect : suspects) {
    if (std::ranges::any_of(running, [&](const std::string& image) {
          return equal_ignoring_case(image, suspect);
        })) {
      found.emplace_back(suspect);
    }
  }
  return found;
}

std::optional<double> cpu_percent(CpuTimes before, CpuTimes after) {
  if (after.idle < before.idle || after.busy < before.busy) {
    return std::nullopt;
  }
  const std::uint64_t busy = after.busy - before.busy;
  const std::uint64_t total = busy + (after.idle - before.idle);
  if (total == 0) {
    return std::nullopt;
  }
  return 100.0 * static_cast<double>(busy) / static_cast<double>(total);
}

}  // namespace myclean::input
