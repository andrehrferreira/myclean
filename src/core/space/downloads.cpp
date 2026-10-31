#include "core/space/downloads.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::space {

namespace {

std::string_view trim(std::string_view text) {
  constexpr std::string_view blanks = " \t\r\n";
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

}  // namespace

Origin parse_zone_identifier(std::string_view text) {
  Origin origin;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::string_view line = trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      continue;  // a section header such as [ZoneTransfer]
    }
    const std::string key = lower_ascii(trim(line.substr(0, equals)));
    const std::string_view value = trim(line.substr(equals + 1));
    if (key == "zoneid") {
      int zone = 0;
      const auto [next, error] = std::from_chars(value.data(), value.data() + value.size(), zone);
      if (error == std::errc{} && next == value.data() + value.size()) {
        origin.zone_id = zone;
      }
    } else if (key == "hosturl") {
      origin.host_url = value;
    } else if (key == "referrerurl") {
      origin.referrer_url = value;
    }
  }
  return origin;
}

std::vector<OldDownload> old_downloads(const ScanResult& downloads, WallTime now,
                                       const IZoneIdentifierReader& zones,
                                       std::chrono::days min_age) {
  std::vector<OldDownload> old;
  for (const Folder& folder : downloads.folders) {
    for (const FileItem& file : folder.files) {
      if (file.link || file.extra_link) {
        continue;
      }
      const auto age = now - std::max(file.created, file.modified);
      if (age <= min_age) {
        continue;
      }
      OldDownload item{.path = path_of(folder, file),
                       .size = file.size,
                       .size_on_disk = file.size_on_disk,
                       .created = file.created,
                       .modified = file.modified,
                       .age_days = std::chrono::floor<std::chrono::days>(age).count()};
      if (!file.placeholder) {
        const Result<std::optional<std::string>> zone = zones.read(item.path);
        if (zone && *zone) {
          item.origin = parse_zone_identifier(**zone);
        }
      }
      old.push_back(std::move(item));
    }
  }
  std::ranges::sort(old, [](const OldDownload& left, const OldDownload& right) {
    return left.size_on_disk != right.size_on_disk ? left.size_on_disk > right.size_on_disk
                                                   : left.path < right.path;
  });
  return old;
}

}  // namespace myclean::space
