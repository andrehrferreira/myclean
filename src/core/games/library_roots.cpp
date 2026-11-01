#include "core/games/library_roots.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/games/epic.hpp"
#include "core/games/paths.hpp"
#include "core/games/steam.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"

namespace myclean::games {

namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 5> source_names = {"owner", "steam", "epic", "gog", "xbox"};

void add_steam_roots(const LauncherData& data, const IGameFiles& files,
                     std::vector<LibraryRoot>& roots, std::vector<std::string>& problems) {
  for (const std::string& text : data.steam_library_files) {
    const Result<std::vector<fs::path>> libraries = parse_library_folders(text);
    if (!libraries) {
      problems.push_back("libraryfolders.vdf: " + libraries.error().message);
      continue;
    }
    for (const fs::path& library : *libraries) {
      // Entries outlive their drives (the backup drive L: is listed): only existing ones count.
      if (const fs::path common = steam_common_folder(library);
          library.is_absolute() && files.is_directory(common)) {
        roots.push_back(LibraryRoot{.path = common, .source = RootSource::steam});
      }
    }
  }
}

void add_epic_roots(const LauncherData& data, const IGameFiles& files,
                    std::vector<LibraryRoot>& roots, std::vector<std::string>& problems) {
  for (const std::string& text : data.epic_manifests) {
    const Result<EpicManifest> manifest = parse_epic_manifest(text);
    if (!manifest) {
      problems.push_back("Epic manifest: " + manifest.error().message);
      continue;
    }
    if (is_epic_game(*manifest) && manifest->install_location.is_absolute() &&
        files.is_directory(manifest->install_location)) {
      roots.push_back(LibraryRoot{.path = manifest->install_location, .source = RootSource::epic});
    }
  }
}

// <drive>\XboxGames counts when an installed title holds MicrosoftGame.config, which the Xbox
// app places in the title's folder or its Content folder.
void add_xbox_roots(const LauncherData& data, const IGameFiles& files,
                    std::vector<LibraryRoot>& roots) {
  constexpr std::string_view marker = "MicrosoftGame.config";
  for (const fs::path& drive : data.drive_roots) {
    const fs::path folder = drive / "XboxGames";
    if (!drive.is_absolute() || !files.is_directory(folder)) {
      continue;
    }
    const std::vector<fs::path> titles = files.subfolders(folder);
    if (std::ranges::any_of(titles, [&files, marker](const fs::path& title) {
          return files.is_file(title / marker) || files.is_file(title / "Content" / marker);
        })) {
      roots.push_back(LibraryRoot{.path = folder, .source = RootSource::xbox});
    }
  }
}

}  // namespace

bool FileSystemGameFiles::is_directory(const fs::path& path) const {
  const Result<os::FileStat> stat = files_->stat(path);
  return stat && stat->kind == os::EntryKind::directory;
}

bool FileSystemGameFiles::is_file(const fs::path& path) const {
  const Result<os::FileStat> stat = files_->stat(path);
  return stat && stat->kind == os::EntryKind::file;
}

std::vector<fs::path> FileSystemGameFiles::subfolders(const fs::path& path) const {
  std::vector<fs::path> folders;
  const Result<os::FileStat> stat = files_->stat(path);
  if (!stat || stat->attributes.reparse_point) {
    return folders;
  }
  const Result<std::vector<os::DirEntry>> entries = files_->enumerate(path);
  if (!entries) {
    return folders;
  }
  for (const os::DirEntry& entry : *entries) {
    if (entry.stat.kind == os::EntryKind::directory && !entry.stat.attributes.reparse_point) {
      folders.push_back(entry.path);
    }
  }
  return folders;
}

std::string_view to_string(RootSource source) {
  return detail::enum_name(source_names, source);
}

RootsReport find_library_roots(const LauncherData& data, std::span<const fs::path> owner_roots,
                               const IGameFiles& files) {
  RootsReport report{.problems = data.problems};
  std::vector<LibraryRoot> roots;
  for (const fs::path& root : owner_roots) {
    if (root.is_absolute()) {
      roots.push_back(LibraryRoot{.path = root, .source = RootSource::owner});
    } else {
      report.problems.push_back("owner root is not an absolute path: " + utf8(root));
    }
  }
  add_steam_roots(data, files, roots, report.problems);
  add_epic_roots(data, files, roots, report.problems);
  for (const fs::path& folder : data.gog_folders) {
    if (folder.is_absolute() && files.is_directory(folder)) {
      roots.push_back(LibraryRoot{.path = folder, .source = RootSource::gog});
    }
  }
  add_xbox_roots(data, files, roots);
  report.roots = LibraryRoots{roots}.roots();
  return report;
}

LibraryRoots::LibraryRoots(std::span<const LibraryRoot> roots) {
  for (const LibraryRoot& root : roots) {
    if (!root.path.is_absolute() || in_windows_apps(root.path) ||
        std::ranges::any_of(
            roots_, [&root](const LibraryRoot& kept) { return same_path(kept.path, root.path); })) {
      continue;
    }
    roots_.push_back(root);
  }
}

const LibraryRoot* LibraryRoots::find(const fs::path& image) const {
  if (in_windows_apps(image)) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      roots_, [&image](const LibraryRoot& root) { return path_within(image, root.path); });
  return found == roots_.end() ? nullptr : &*found;
}

}  // namespace myclean::games
