#include "core/scanall/owner_decisions.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/health/analysis.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/scanall/findings.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;

constexpr std::array<placement::DriveClass, 1> hdd_classes = {placement::DriveClass::hdd};

using Components = std::vector<std::wstring>;

// The non-empty components of a path, split at '\' and '/'.
Components split(std::wstring_view text) {
  Components parts;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = std::min(text.find_first_of(L"\\/", start), text.size());
    if (end > start) {
      parts.emplace_back(text.substr(start, end - start));
    }
    start = end + 1;
  }
  return parts;
}

fs::path from_utf8(std::string_view text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// A string value of `key`; empty when it is missing or not text.
Result<std::string> text_value(const placement::IRegistry& registry, std::string_view key,
                               std::string_view name) {
  Result<std::optional<placement::RegistryValue>> value = registry.read(key, name);
  if (!value) {
    return tl::unexpected(value.error());
  }
  const std::optional<placement::RegistryValue>& found = *value;
  if (!found || found->type == placement::RegistryType::dword) {
    return std::string{};
  }
  return found->text;
}

// A DWORD value of `key`; nullopt when it is missing or not a number.
Result<std::optional<std::uint64_t>> number_value(const placement::IRegistry& registry,
                                                  std::string_view key, std::string_view name) {
  Result<std::optional<placement::RegistryValue>> value = registry.read(key, name);
  if (!value) {
    return tl::unexpected(value.error());
  }
  const std::optional<placement::RegistryValue>& found = *value;
  if (!found || found->type != placement::RegistryType::dword) {
    return std::optional<std::uint64_t>{};
  }
  std::uint64_t number = 0;
  const std::string& text = found->text;
  const char* last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(text.data(), last, number);
  if (error != std::errc{} || end != last) {
    return std::optional<std::uint64_t>{};
  }
  return std::optional<std::uint64_t>{number};
}

// Whether `location` lies at or below a path matching `pattern` (both split into components).
bool at_or_below(const Components& location, const Components& pattern) {
  if (pattern.empty() || location.size() < pattern.size()) {
    return false;
  }
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    if (!clean::wildcard_match(location[index], pattern[index])) {
      return false;
    }
  }
  return true;
}

// The first uninstall area of `areas` with a location that holds `location`; null when none.
const map::Area* area_of(const fs::path& location, std::span<const map::Area> areas,
                         const clean::IEnvironment& environment) {
  const Components components = split(location.native());
  for (const map::Area& area : areas) {
    if (area.reclaim.kind != map::ReclaimKind::uninstall) {
      continue;
    }
    for (const std::wstring& path : area.paths) {
      for (const std::wstring& expanded : clean::expand_all(path, environment)) {
        if (at_or_below(components, split(expanded))) {
          return &area;
        }
      }
    }
  }
  return nullptr;
}

}  // namespace

fs::path archive_folder(const fs::path& source, const fs::path& root) {
  const std::wstring drive = placement::drive_root(source);
  if (drive.empty()) {
    return {};
  }
  return root / L"Archive" / drive.substr(0, 1) / source.relative_path();
}

std::optional<fs::path> archive_target(std::span<const DriveInfo> drives, const fs::path& source,
                                       Bytes size) {
  const DriveInfo* target = best_target(drives, source, size, hdd_classes);
  if (target == nullptr) {
    return std::nullopt;
  }
  return archive_folder(source, target->root);
}

std::vector<DriveInfo> drive_infos(std::span<const placement::TargetVolume> volumes) {
  std::vector<DriveInfo> drives;
  drives.reserve(volumes.size());
  for (const placement::TargetVolume& volume : volumes) {
    drives.push_back(
        DriveInfo{.root = volume.root,
                  .drive_class = volume.drive_class,
                  .health = volume.healthy ? health::Status::good : health::Status::unknown,
                  .health_note = volume.health_note,
                  .size = volume.size,
                  .free = volume.free});
  }
  return drives;
}

Result<std::optional<UninstallEntry>> uninstall_entry(const placement::IRegistry& registry,
                                                      std::string_view key) {
  UninstallEntry entry{.key = std::string(key)};
  const std::array<std::pair<std::string_view, std::string*>, 4> texts = {{
      {"DisplayName", &entry.name},
      {"Publisher", &entry.publisher},
      {"DisplayVersion", &entry.version},
      {"UninstallString", &entry.uninstall},
  }};
  for (const auto& [name, field] : texts) {
    Result<std::string> value = text_value(registry, key, name);
    if (!value) {
      return tl::unexpected(value.error());
    }
    *field = *std::move(value);
  }
  if (entry.uninstall.empty()) {
    Result<std::string> quiet = text_value(registry, key, "QuietUninstallString");
    if (!quiet) {
      return tl::unexpected(quiet.error());
    }
    entry.uninstall = *std::move(quiet);
  }
  if (entry.name.empty() || entry.uninstall.empty()) {
    return std::optional<UninstallEntry>{};
  }
  Result<std::string> location = text_value(registry, key, "InstallLocation");
  if (!location) {
    return tl::unexpected(location.error());
  }
  std::string text = *std::move(location);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text = text.substr(1, text.size() - 2);
  }
  while (text.size() > 3 && (text.back() == '\\' || text.back() == '/')) {
    text.pop_back();
  }
  entry.location = from_utf8(text);
  Result<std::optional<std::uint64_t>> size = number_value(registry, key, "EstimatedSize");
  if (!size) {
    return tl::unexpected(size.error());
  }
  entry.estimated = Bytes{size->value_or(0) * 1024};
  return std::optional<UninstallEntry>{std::move(entry)};
}

Result<std::vector<UninstallEntry>> uninstall_entries(const placement::IRegistry& registry) {
  std::vector<UninstallEntry> entries;
  for (const std::string_view root : uninstall_roots) {
    Result<std::vector<std::string>> subkeys = registry.subkeys(root);
    if (!subkeys) {
      return tl::unexpected(subkeys.error());
    }
    for (const std::string& subkey : *subkeys) {
      const std::string key = std::string(root) + "\\" + subkey;
      Result<std::optional<std::uint64_t>> system = number_value(registry, key, "SystemComponent");
      Result<std::string> parent = text_value(registry, key, "ParentKeyName");
      if (!system || !parent) {
        return tl::unexpected(system ? parent.error() : system.error());
      }
      if (system->value_or(0) != 0 || !parent->empty()) {
        continue;
      }
      Result<std::optional<UninstallEntry>> entry = uninstall_entry(registry, key);
      if (!entry) {
        return tl::unexpected(entry.error());
      }
      if (*entry) {
        entries.push_back(**std::move(entry));
      }
    }
  }
  return entries;
}

std::vector<ProgramFinding> uninstall_review(std::span<const UninstallEntry> programs,
                                             std::span<const map::Area> areas,
                                             const clean::IEnvironment& environment) {
  std::vector<ProgramFinding> findings;
  for (const UninstallEntry& program : programs) {
    if (program.location.empty()) {
      continue;
    }
    if (const map::Area* area = area_of(program.location, areas, environment)) {
      findings.push_back(
          ProgramFinding{.program = program, .area = area->label, .note = area->reclaim.note});
    }
  }
  return findings;
}

}  // namespace myclean::scanall
