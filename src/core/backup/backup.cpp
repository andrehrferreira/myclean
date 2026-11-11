#include "core/backup/backup.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/alerts/engine.hpp"
#include "core/backup/repos.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/space/scanner.hpp"
#include "core/store/database.hpp"

namespace myclean::backup {

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

// How many artifacts a source lists in the answer; the newest first.
constexpr std::size_t listed_artifacts = 50;

constexpr std::array<std::string_view, 15> products = {"Macrium Reflect",
                                                       "Veeam Agent",
                                                       "Acronis",
                                                       "Backblaze",
                                                       "Duplicati",
                                                       "AOMEI Backupper",
                                                       "EaseUS Todo Backup",
                                                       "Paragon Backup",
                                                       "Cobian",
                                                       "Carbonite",
                                                       "IDrive",
                                                       "Arq",
                                                       "Kopia",
                                                       "Duplicacy",
                                                       "UrBackup"};

std::string lower(std::string_view text) {
  std::string folded(text);
  std::ranges::transform(folded, folded.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return folded;
}

std::wstring lower(std::wstring text) {
  std::ranges::transform(text, text.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
  });
  return text;
}

bool contains(std::string_view text, std::string_view part) {
  return lower(text).find(lower(part)) != std::string::npos;
}

bool ends_with(const std::wstring& folded, std::wstring_view suffix) {
  return folded.size() >= suffix.size() && folded.ends_with(suffix);
}

std::string drive_of(const fs::path& root) {
  std::string text = utf8_text(root);
  while (!text.empty() && (text.back() == '\\' || text.back() == '/')) {
    text.pop_back();
  }
  return text;
}

// The digits of `text` at [at, at + count) as a number; nullopt when one is no digit.
std::optional<int> digits(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (std::size_t index = at; index < at + count; ++index) {
    if (std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
      return std::nullopt;
    }
    value = value * 10 + (text[index] - '0');
  }
  return value;
}

bool is_digit_at(std::string_view text, std::size_t at) {
  return at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0;
}

bool separator(char c) {
  return c == '-' || c == '_' || c == '.';
}

std::optional<WallTime> date_of(int year, int month, int day) {
  constexpr int first_year = 1990;
  constexpr int last_year = 2100;
  if (year < first_year || year > last_year) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{std::chrono::year{year},
                                         std::chrono::month{static_cast<unsigned>(month)},
                                         std::chrono::day{static_cast<unsigned>(day)}};
  if (!date.ok()) {
    return std::nullopt;
  }
  return std::chrono::sys_days{date};
}

// The date starting at `at` (not preceded by a digit), or nullopt.
std::optional<WallTime> date_at(std::string_view text, std::size_t at) {
  if (at > 0 && is_digit_at(text, at - 1)) {
    return std::nullopt;
  }
  // dd-mm-yyyy
  if (at + 10 <= text.size() && separator(text[at + 2]) && separator(text[at + 5]) &&
      !is_digit_at(text, at + 10)) {
    const auto day = digits(text, at, 2);
    const auto month = digits(text, at + 3, 2);
    const auto year = digits(text, at + 6, 4);
    if (day && month && year) {
      return date_of(*year, *month, *day);
    }
  }
  // yyyy-mm-dd
  if (at + 10 <= text.size() && separator(text[at + 4]) && separator(text[at + 7]) &&
      !is_digit_at(text, at + 10)) {
    const auto year = digits(text, at, 4);
    const auto month = digits(text, at + 5, 2);
    const auto day = digits(text, at + 8, 2);
    if (year && month && day) {
      return date_of(*year, *month, *day);
    }
  }
  // yyyymmdd
  if (!is_digit_at(text, at + 8)) {
    const auto year = digits(text, at, 4);
    const auto month = digits(text, at + 4, 2);
    const auto day = digits(text, at + 6, 2);
    if (year && month && day) {
      return date_of(*year, *month, *day);
    }
  }
  return std::nullopt;
}

std::optional<ArtifactKind> file_kind(const fs::path& name) {
  const std::wstring folded = lower(name.native());
  if (ends_with(folded, L".mrimg")) {
    return ArtifactKind::macrium_image;
  }
  if (ends_with(folded, L".vbk")) {
    return ArtifactKind::veeam_backup;
  }
  if (ends_with(folded, L".tibx") || ends_with(folded, L".tib")) {
    return ArtifactKind::acronis_backup;
  }
  if (ends_with(folded, L".dump") || ends_with(folded, L"_gitlab_backup.tar") ||
      ends_with(folded, L".bak")) {
    return ArtifactKind::dump;
  }
  return std::nullopt;
}

std::optional<ArtifactKind> folder_kind(const fs::path& name, int depth) {
  const std::wstring folded = lower(name.native());
  if (folded == L"windowsimagebackup") {
    return ArtifactKind::system_image;
  }
  if (folded == L"filehistory") {
    return ArtifactKind::file_history;
  }
  const std::string text = utf8_text(name);
  if (depth == 1 && contains(text, "backup") && name_date(text)) {
    return ArtifactKind::manual_copy;
  }
  return std::nullopt;
}

// The newest last write of `folder` and its entries; `fallback` when it cannot be listed.
WallTime newest_in(const space::IDirectoryReader& reader, const fs::path& folder,
                   WallTime fallback) {
  Result<std::vector<space::ListedEntry>> entries = reader.list(folder);
  if (!entries) {
    return fallback;
  }
  WallTime newest = fallback;
  for (const space::ListedEntry& entry : *entries) {
    newest = std::max(newest, entry.modified);
  }
  return newest;
}

// What an entry at `depth` below a volume root is: an artifact, a folder to enter, or neither.
struct Visit {
  std::optional<Artifact> artifact;
  bool enter = false;
};

Visit visit(const space::IDirectoryReader& reader, const space::ListedEntry& entry,
            const fs::path& path, int depth) {
  if (!entry.directory) {
    const std::optional<ArtifactKind> kind = file_kind(entry.name);
    return kind ? Visit{.artifact = Artifact{.kind = *kind, .path = path, .newest = entry.modified}}
                : Visit{};
  }
  if (entry.attributes.reparse_point) {
    return {};
  }
  const std::optional<ArtifactKind> kind = folder_kind(entry.name, depth);
  if (!kind) {
    return Visit{.enter = true};
  }
  if (*kind == ArtifactKind::system_image && depth == 1) {
    return {};  // the volume's own WindowsImageBackup: the system_image source reports it
  }
  const WallTime newest = *kind == ArtifactKind::manual_copy
                              ? name_date(utf8_text(entry.name)).value_or(entry.created)
                              : newest_in(reader, path, entry.modified);
  return Visit{.artifact = Artifact{.kind = *kind, .path = path, .newest = newest}};
}

std::string artifact_name(ArtifactKind kind) {
  switch (kind) {
    case ArtifactKind::macrium_image:
      return "Macrium Reflect image";
    case ArtifactKind::veeam_backup:
      return "Veeam backup";
    case ArtifactKind::acronis_backup:
      return "Acronis backup";
    case ArtifactKind::system_image:
      return "system image";
    case ArtifactKind::file_history:
      return "File History copy";
    case ArtifactKind::manual_copy:
      return "manual copy";
    case ArtifactKind::dump:
      return "database or app dump";
  }
  return "backup";
}

std::string source_name(SourceKind kind) {
  switch (kind) {
    case SourceKind::file_history:
      return "File History";
    case SourceKind::system_image:
      return "System image";
    case SourceKind::restore_points:
      return "Restore points";
    case SourceKind::shadow_copies:
      return "Shadow copies";
    case SourceKind::onedrive:
      return "OneDrive folder backup";
    case SourceKind::third_party:
      return "Third-party backup tools";
    case SourceKind::backup_drives:
      return "Backup drives";
  }
  return "backup";
}

Source unreadable(SourceKind kind, const Error& error) {
  return Source{.kind = kind, .status = Status::unknown, .detail = error.message};
}

Source file_history_source(const IBackupSystem& system) {
  Result<FileHistoryFacts> facts = system.file_history();
  if (!facts) {
    return unreadable(SourceKind::file_history, facts.error());
  }
  if (!facts->configured) {
    return Source{.kind = SourceKind::file_history, .detail = "File History is not turned on"};
  }
  Source source{.kind = SourceKind::file_history,
                .status = Status::found,
                .location = facts->target,
                .newest = facts->newest,
                .copies = facts->newest ? 1U : 0U};
  if (facts->target.empty()) {
    source.detail = "turned on; its target could not be read";
  } else if (!facts->newest) {
    source.detail = "turned on; no copy time recorded yet";
  }
  return source;
}

Source image_source(const IBackupSystem& system, const space::IDirectoryReader& reader,
                    std::span<const Volume> volumes) {
  Source source{.kind = SourceKind::system_image};
  for (const Volume& volume : volumes) {
    if (std::optional<Artifact> folder = image_folder(reader, volume.root)) {
      source.artifacts.push_back(*std::move(folder));
    }
  }
  if (!source.artifacts.empty()) {
    std::ranges::sort(source.artifacts, std::ranges::greater{}, &Artifact::newest);
    source.status = Status::found;
    source.copies = source.artifacts.size();
    source.newest = source.artifacts.front().newest;
    source.location = utf8_text(source.artifacts.front().path);
    return source;
  }
  Result<bool> versions = system.image_versions();
  if (!versions) {
    return unreadable(SourceKind::system_image, versions.error());
  }
  if (*versions) {
    source.status = Status::found;
    source.detail =
        "wbadmin lists a system image version, but no WindowsImageBackup folder is on a local "
        "drive (a network or removed target); its time is unknown";
  }
  return source;
}

Source restore_source(const IBackupSystem& system) {
  Result<std::vector<RestorePoint>> points = system.restore_points();
  if (!points) {
    return unreadable(SourceKind::restore_points, points.error());
  }
  Source source{.kind = SourceKind::restore_points,
                .status = points->empty() ? Status::not_found : Status::found,
                .location = points->empty() ? std::string{} : "the system drive",
                .copies = points->size(),
                .detail = "kept on the drive they protect, so they are no backup"};
  for (const RestorePoint& point : *points) {
    source.newest = std::max(source.newest.value_or(point.created), point.created);
  }
  return source;
}

Source shadow_source(const IBackupSystem& system) {
  Result<std::vector<ShadowCopy>> copies = system.shadow_copies();
  if (!copies) {
    return unreadable(SourceKind::shadow_copies, copies.error());
  }
  Source source{.kind = SourceKind::shadow_copies,
                .status = copies->empty() ? Status::not_found : Status::found,
                .copies = copies->size(),
                .detail = "kept on the drive they protect, so they are no backup"};
  std::vector<std::string> volumes;
  for (const ShadowCopy& copy : *copies) {
    source.newest = std::max(source.newest.value_or(copy.created), copy.created);
    if (std::ranges::find(volumes, copy.volume) == volumes.end()) {
      volumes.push_back(copy.volume);
    }
  }
  for (const std::string& volume : volumes) {
    source.location += (source.location.empty() ? "" : ", ") + volume;
  }
  return source;
}

Source onedrive_source(const IBackupSystem& system) {
  Result<std::vector<OneDriveAccount>> accounts = system.onedrive();
  if (!accounts) {
    return unreadable(SourceKind::onedrive, accounts.error());
  }
  Source source{.kind = SourceKind::onedrive,
                .detail = accounts->empty() ? "OneDrive is not set up"
                                            : "no known folder is backed up to OneDrive"};
  for (const OneDriveAccount& account : *accounts) {
    if (account.protected_folders.empty()) {
      continue;
    }
    source.status = Status::found;
    source.location = utf8_text(account.folder);
    source.names = account.protected_folders;
    source.copies = account.protected_folders.size();
    source.detail = "continuous sync (time unknown)";
    if (account.sign_in_error) {
      source.detail += "; OneDrive has been in a sign-in error state since " +
                       std::to_string(store::to_unix_seconds(*account.sign_in_error)) +
                       " (Unix seconds), so nothing is uploaded";
    }
    break;
  }
  return source;
}

Source product_source(const IBackupSystem& system) {
  Result<std::vector<std::string>> programs = system.installed_programs();
  if (!programs) {
    return unreadable(SourceKind::third_party, programs.error());
  }
  Source source{.kind = SourceKind::third_party};
  for (const std::string& program : *programs) {
    if (const std::optional<std::string_view> product = backup_product(program);
        product && std::ranges::find(source.names, program) == source.names.end()) {
      source.names.push_back(program);
    }
  }
  std::ranges::sort(source.names);
  source.copies = source.names.size();
  source.status = source.names.empty() ? Status::not_found : Status::found;
  source.detail = source.names.empty() ? "no backup product is installed"
                                       : "installed; their copies are counted under backup_drives";
  return source;
}

Source drives_source(const space::IDirectoryReader& reader, std::span<const Volume> volumes,
                     const ScanLimits& limits, const std::stop_token& stop) {
  const std::vector<Volume> backup = backup_volumes(volumes);
  Source source{.kind = SourceKind::backup_drives};
  std::size_t unreadable_folders = 0;
  bool truncated = false;
  for (const Volume& volume : backup) {
    source.location +=
        (source.location.empty() ? "" : ", ") + drive_of(volume.root) + " \"" + volume.label + "\"";
    ArtifactScan scan = scan_artifacts(reader, volume.root, limits, stop);
    unreadable_folders += scan.unreadable;
    truncated = truncated || scan.truncated;
    std::ranges::move(scan.artifacts, std::back_inserter(source.artifacts));
  }
  std::ranges::sort(source.artifacts, std::ranges::greater{}, &Artifact::newest);
  source.copies = source.artifacts.size();
  source.status = source.artifacts.empty() ? Status::not_found : Status::found;
  if (!source.artifacts.empty()) {
    source.newest = source.artifacts.front().newest;
  }
  if (backup.empty()) {
    source.detail = "no drive is labelled as a backup drive";
  } else if (truncated || unreadable_folders > 0) {
    source.detail = std::to_string(unreadable_folders) + " folders could not be read" +
                    (truncated ? "; the walk stopped at its folder limit" : "");
  }
  return source;
}

std::optional<LastBackup> last_backup(std::span<const Source> sources) {
  std::optional<LastBackup> last;
  for (const Source& source : sources) {
    if (!source.counts() || source.status != Status::found || !source.newest ||
        (last && last->newest >= *source.newest)) {
      continue;
    }
    LastBackup candidate{.source = source.kind,
                         .name = source_name(source.kind),
                         .location = source.location,
                         .newest = *source.newest};
    if (!source.artifacts.empty() && source.artifacts.front().newest == *source.newest) {
      const Artifact& artifact = source.artifacts.front();
      candidate.name = artifact_name(artifact.kind);
      candidate.location = utf8_text(artifact.path);
    }
    last = std::move(candidate);
  }
  return last;
}

std::int64_t age_days(WallTime newest, WallTime now) {
  return std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::days>(now - newest).count());
}

Json time_json(const std::optional<WallTime>& time) {
  return time ? Json(store::to_unix_seconds(*time)) : Json(nullptr);
}

Json text_json(const std::string& text) {
  return text.empty() ? Json(nullptr) : Json(text);
}

Json source_json(const Source& source, WallTime now) {
  Json json{
      {"source", to_string(source.kind)},
      {"name", source_name(source.kind)},
      {"status", to_string(source.status)},
      {"location", text_json(source.location)},
      {"newest", time_json(source.newest)},
      {"newest_age_days", source.newest ? Json(age_days(*source.newest, now)) : Json(nullptr)},
      {"copies", source.copies},
      {"counts_toward_age", source.counts()},
      {"detail", text_json(source.detail)}};
  if (!source.names.empty()) {
    json[source.kind == SourceKind::onedrive ? "folders" : "products"] = source.names;
  }
  if (!source.artifacts.empty()) {
    Json artifacts = Json::array();
    for (std::size_t index = 0; index < std::min(source.artifacts.size(), listed_artifacts);
         ++index) {
      const Artifact& artifact = source.artifacts[index];
      artifacts.push_back(Json{{"kind", to_string(artifact.kind)},
                               {"path", utf8_text(artifact.path)},
                               {"newest", store::to_unix_seconds(artifact.newest)}});
    }
    json["artifacts"] = std::move(artifacts);
  }
  return json;
}

Json repos_json(const RepoReport& report) {
  Json unprotected = Json::array();
  for (const RepoFinding& finding : report.unprotected) {
    unprotected.push_back(Json{{"path", utf8_text(finding.path)},
                               {"no_remote", finding.no_remote},
                               {"unpushed", finding.unpushed},
                               {"uncommitted", finding.uncommitted},
                               {"text", finding.text}});
  }
  return Json{{"checked", report.checked},
              {"unprotected", std::move(unprotected)},
              {"problems", report.problems},
              {"truncated", report.truncated}};
}

}  // namespace

std::string_view to_string(SourceKind kind) {
  switch (kind) {
    case SourceKind::file_history:
      return "file_history";
    case SourceKind::system_image:
      return "system_image";
    case SourceKind::restore_points:
      return "restore_points";
    case SourceKind::shadow_copies:
      return "shadow_copies";
    case SourceKind::onedrive:
      return "onedrive";
    case SourceKind::third_party:
      return "third_party";
    case SourceKind::backup_drives:
      return "backup_drives";
  }
  return "unknown";
}

std::string_view to_string(Status status) {
  switch (status) {
    case Status::found:
      return "found";
    case Status::not_found:
      return "not_found";
    case Status::unknown:
      return "could_not_check";
  }
  return "could_not_check";
}

std::string_view to_string(ArtifactKind kind) {
  switch (kind) {
    case ArtifactKind::macrium_image:
      return "macrium_image";
    case ArtifactKind::veeam_backup:
      return "veeam_backup";
    case ArtifactKind::acronis_backup:
      return "acronis_backup";
    case ArtifactKind::system_image:
      return "system_image";
    case ArtifactKind::file_history:
      return "file_history";
    case ArtifactKind::manual_copy:
      return "manual_copy";
    case ArtifactKind::dump:
      return "dump";
  }
  return "dump";
}

bool Source::counts() const {
  return kind != SourceKind::restore_points && kind != SourceKind::shadow_copies;
}

std::span<const std::string_view> backup_products() {
  return products;
}

std::optional<std::string_view> backup_product(std::string_view display_name) {
  for (const std::string_view product : products) {
    // "Arq" and "Cobian" are short: match them as whole words at the start of the name.
    if (product.size() <= 6 ? lower(display_name).starts_with(lower(product) + " ") ||
                                  lower(display_name) == lower(product)
                            : contains(display_name, product)) {
      return product;
    }
  }
  return std::nullopt;
}

std::optional<WallTime> name_date(std::string_view name) {
  for (std::size_t at = 0; at < name.size(); ++at) {
    if (is_digit_at(name, at)) {
      if (std::optional<WallTime> date = date_at(name, at)) {
        return date;
      }
    }
  }
  return std::nullopt;
}

std::vector<Volume> backup_volumes(std::span<const Volume> volumes) {
  std::vector<Volume> found;
  std::ranges::copy_if(volumes, std::back_inserter(found),
                       [](const Volume& volume) { return contains(volume.label, "backup"); });
  return found;
}

ArtifactScan scan_artifacts(const space::IDirectoryReader& reader, const fs::path& root,
                            const ScanLimits& limits, const std::stop_token& stop) {
  struct Pending {
    fs::path folder;
    int depth = 0;
  };
  ArtifactScan scan;
  std::deque<Pending> pending{Pending{.folder = root, .depth = 0}};
  std::size_t listed = 0;
  while (!pending.empty() && !stop.stop_requested()) {
    if (listed == limits.max_folders) {
      scan.truncated = true;
      break;
    }
    const Pending next = std::move(pending.front());
    pending.pop_front();
    Result<std::vector<space::ListedEntry>> entries = reader.list(next.folder);
    ++listed;
    if (!entries) {
      ++scan.unreadable;
      continue;
    }
    for (const space::ListedEntry& entry : *entries) {
      const fs::path path = next.folder / entry.name;
      Visit visited = visit(reader, entry, path, next.depth + 1);
      if (visited.artifact) {
        scan.artifacts.push_back(*std::move(visited.artifact));
      } else if (visited.enter && next.depth + 1 < limits.max_depth) {
        pending.push_back(Pending{.folder = path, .depth = next.depth + 1});
      }
    }
  }
  return scan;
}

std::optional<Artifact> image_folder(const space::IDirectoryReader& reader, const fs::path& root) {
  Result<std::vector<space::ListedEntry>> entries = reader.list(root);
  if (!entries) {
    return std::nullopt;
  }
  const auto folder = std::ranges::find_if(*entries, [](const space::ListedEntry& entry) {
    return entry.directory && !entry.attributes.reparse_point &&
           lower(entry.name.native()) == L"windowsimagebackup";
  });
  if (folder == entries->end()) {
    return std::nullopt;
  }
  const fs::path path = root / folder->name;
  WallTime newest = folder->modified;
  if (Result<std::vector<space::ListedEntry>> computers = reader.list(path); computers) {
    for (const space::ListedEntry& computer : *computers) {
      if (computer.directory && !computer.attributes.reparse_point) {
        newest = std::max(newest, newest_in(reader, path / computer.name, computer.modified));
      }
    }
  }
  return Artifact{.kind = ArtifactKind::system_image, .path = path, .newest = newest};
}

Recommendation recommend(std::span<const placement::TargetVolume> volumes,
                         std::span<const Volume> labels, const fs::path& system_root) {
  const auto labelled = [&](const fs::path& root) {
    return std::ranges::any_of(labels, [&](const Volume& volume) {
      return lower(volume.root.native()) == lower(root.native()) &&
             contains(volume.label, "backup");
    });
  };
  std::vector<const placement::TargetVolume*> healthy;
  Recommendation advice;
  for (const placement::TargetVolume& volume : volumes) {
    if (lower(volume.root.native()) == lower(system_root.native()) ||
        (volume.drive_class != placement::DriveClass::hdd && !labelled(volume.root))) {
      continue;
    }
    if (!volume.healthy) {
      advice.left_out.push_back(DriveAdvice{
          .drive = drive_of(volume.root),
          .reason = "its health is not good" +
                    (volume.health_note.empty() ? std::string{} : ": " + volume.health_note)});
      continue;
    }
    healthy.push_back(&volume);
  }
  std::ranges::stable_sort(healthy, std::ranges::greater{},
                           [](const placement::TargetVolume* volume) { return volume->free; });
  if (healthy.empty()) {
    advice.text =
        "No backup exists and no healthy backup drive is attached. Attach an external drive, "
        "then turn on File History to it (Settings > System > Storage > Advanced storage "
        "settings > Backup options) and make a system image to it each month (Control Panel > "
        "Backup and Restore (Windows 7) > Create a system image). MyClean changes no backup "
        "setting itself.";
    return advice;
  }
  const std::string history = drive_of(healthy.front()->root);
  const std::string image = drive_of(healthy.size() > 1 ? healthy[1]->root : healthy.front()->root);
  advice.named.push_back(history);
  if (image != history) {
    advice.named.push_back(image);
  }
  advice.text = "No backup exists. Turn on File History to " + history +
                " (Settings > System > Storage > Advanced storage settings > Backup options, or "
                "Control Panel > File History > Select drive) so your files are copied every "
                "hour, and make a system image to " +
                image +
                " each month (Control Panel > Backup and Restore (Windows 7) > Create a system "
                "image). MyClean changes no backup setting itself.";
  return advice;
}

Report check(const Probes& probes, WallTime now, const std::stop_token& stop,
             const ScanLimits& limits) {
  Report report{.checked = now};
  const IBackupSystem& system = *probes.system;
  Result<std::vector<Volume>> volumes = system.volumes();
  const std::vector<Volume> known = volumes ? *volumes : std::vector<Volume>{};
  report.sources.push_back(file_history_source(system));
  report.sources.push_back(image_source(system, *probes.reader, known));
  report.sources.push_back(restore_source(system));
  report.sources.push_back(shadow_source(system));
  report.sources.push_back(onedrive_source(system));
  report.sources.push_back(product_source(system));
  report.sources.push_back(volumes ? drives_source(*probes.reader, known, limits, stop)
                                   : unreadable(SourceKind::backup_drives, volumes.error()));
  report.last = last_backup(report.sources);
  report.backup_exists = std::ranges::any_of(report.sources, [](const Source& source) {
    // A product installed is no copy: its copies are the artifacts it wrote.
    return source.counts() && source.kind != SourceKind::third_party &&
           source.status == Status::found;
  });
  if (!report.backup_exists && probes.drives != nullptr) {
    if (Result<std::vector<placement::TargetVolume>> targets = probes.drives->volumes(); targets) {
      report.recommendation = recommend(*targets, known, probes.system_root);
    }
  }
  if (probes.git != nullptr) {
    const RepoWalk walk = find_repositories(*probes.reader, probes.workspace_roots, probes.skip,
                                            probes.repo_limits, stop);
    report.repositories = check_repositories(*probes.git, walk.repositories, stop);
    report.repositories->truncated = walk.truncated;
  }
  return report;
}

Json report_json(const Report& report, WallTime now) {
  Json sources = Json::array();
  for (const Source& source : report.sources) {
    sources.push_back(source_json(source, now));
  }
  Json last = nullptr;
  std::string summary;
  if (report.last) {
    const std::int64_t days = age_days(report.last->newest, now);
    last = Json{{"source", to_string(report.last->source)},
                {"name", report.last->name},
                {"location", text_json(report.last->location)},
                {"newest", store::to_unix_seconds(report.last->newest)},
                {"age_days", days}};
    summary = "newest backup is " + std::to_string(days) + " day" + (days == 1 ? "" : "s") +
              " old (" + report.last->name + ")";
  } else if (report.backup_exists) {
    summary = "a backup exists, but its copy time is unknown";
  } else {
    summary = "no backup exists";
  }
  Json recommendation = nullptr;
  if (report.recommendation) {
    Json left_out = Json::array();
    for (const DriveAdvice& drive : report.recommendation->left_out) {
      left_out.push_back(Json{{"drive", drive.drive}, {"reason", drive.reason}});
    }
    recommendation = Json{{"text", report.recommendation->text},
                          {"drives", report.recommendation->named},
                          {"left_out", std::move(left_out)}};
  }
  return Json{
      {"checked_at", store::to_unix_seconds(report.checked)},
      {"sources", std::move(sources)},
      {"last_backup", std::move(last)},
      {"backup_exists", report.backup_exists},
      {"summary", summary},
      {"recommendation", std::move(recommendation)},
      {"repositories", report.repositories ? repos_json(*report.repositories) : Json(nullptr)}};
}

std::vector<alerts::Reading> age_readings(const Report& report, WallTime now) {
  if (report.last) {
    const std::int64_t days = age_days(report.last->newest, now);
    return {alerts::Reading{.metric = std::string(age_metric),
                            .entity = "newest",
                            .value = static_cast<double>(days),
                            .detail = "newest backup is " + std::to_string(days) + " day" +
                                      (days == 1 ? "" : "s") + " old (" + report.last->name + ")"}};
  }
  if (report.backup_exists) {
    return {};
  }
  return {alerts::Reading{.metric = std::string(age_metric),
                          .entity = "newest",
                          .value = std::numeric_limits<double>::infinity(),
                          .detail = "no backup exists"}};
}

}  // namespace myclean::backup
