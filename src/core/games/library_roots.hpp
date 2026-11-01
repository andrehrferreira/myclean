#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/os/file_system.hpp"

// Game library roots: folders whose every image is a game (spec Game Detection). Built from
// Steam, Epic, GOG and Xbox installs and from the owner's own roots; WindowsApps never is one.
namespace myclean::games {

// The file system questions root discovery asks. No call follows a link's target, reads file
// content or hydrates a cloud file.
class IGameFiles {
 public:
  virtual ~IGameFiles() = default;
  IGameFiles(const IGameFiles&) = delete;
  IGameFiles& operator=(const IGameFiles&) = delete;
  IGameFiles(IGameFiles&&) = delete;
  IGameFiles& operator=(IGameFiles&&) = delete;

  // True when `path` exists and is a folder (a folder link counts, as itself).
  [[nodiscard]] virtual bool is_directory(const std::filesystem::path& path) const = 0;
  // True when `path` exists and is a file.
  [[nodiscard]] virtual bool is_file(const std::filesystem::path& path) const = 0;
  // The folders directly inside `path` that are not links; empty when `path` is a link or
  // cannot be listed.
  [[nodiscard]] virtual std::vector<std::filesystem::path> subfolders(
      const std::filesystem::path& path) const = 0;

 protected:
  IGameFiles() = default;
};

// IGameFiles over an os::IFileSystem (FakeFileSystem in tests). `files` must outlive it.
class FileSystemGameFiles final : public IGameFiles {
 public:
  explicit FileSystemGameFiles(const os::IFileSystem& files) : files_(&files) {}

  [[nodiscard]] bool is_directory(const std::filesystem::path& path) const override;
  [[nodiscard]] bool is_file(const std::filesystem::path& path) const override;
  [[nodiscard]] std::vector<std::filesystem::path> subfolders(
      const std::filesystem::path& path) const override;

 private:
  const os::IFileSystem* files_;
};

enum class RootSource { owner, steam, epic, gog, xbox };

[[nodiscard]] std::string_view to_string(RootSource source);

struct LibraryRoot {
  std::filesystem::path path;
  RootSource source = RootSource::owner;

  friend bool operator==(const LibraryRoot&, const LibraryRoot&) = default;
};

// What the platform reads from the launchers, as found: nothing parsed or checked yet.
struct LauncherData {
  std::vector<std::string> steam_library_files;    // text of each libraryfolders.vdf
  std::vector<std::string> epic_manifests;         // text of each Epic .item manifest
  std::vector<std::filesystem::path> gog_folders;  // GOG.com\Games\*\path
  std::vector<std::filesystem::path> drive_roots;  // "C:\", "E:\": where XboxGames may be
  std::vector<std::string> problems;               // what could not be read, and why
};

struct RootsReport {
  std::vector<LibraryRoot> roots;
  std::vector<std::string> problems;  // LauncherData::problems plus files that failed to parse
};

// The library roots of this machine (spec Game Detection, spike X5 §2), owner roots first:
//   owner  every absolute `owner_roots` entry, existing or not (a drive may be unplugged)
//   steam  <library>\steamapps\common of every libraryfolders.vdf entry, when it exists
//   epic   InstallLocation of every is_epic_game manifest, when it exists
//   gog    every GOG folder that exists
//   xbox   <drive>\XboxGames when one of its subfolders holds MicrosoftGame.config, directly or
//          in its Content folder
// A malformed file adds a problem and nothing else. Duplicates and anything in WindowsApps are
// dropped (see LibraryRoots).
[[nodiscard]] RootsReport find_library_roots(const LauncherData& data,
                                             std::span<const std::filesystem::path> owner_roots,
                                             const IGameFiles& files);

// Matches images against library roots.
class LibraryRoots {
 public:
  LibraryRoots() = default;
  // Keeps the first of duplicate roots and drops relative roots and any root in WindowsApps.
  explicit LibraryRoots(std::span<const LibraryRoot> roots);

  // The root that holds `image` at a folder boundary, case-insensitively; null when none does or
  // the image is in WindowsApps (an owner root "E:\" does not make E:\WindowsApps a library).
  [[nodiscard]] const LibraryRoot* find(const std::filesystem::path& image) const;
  [[nodiscard]] const std::vector<LibraryRoot>& roots() const { return roots_; }

 private:
  std::vector<LibraryRoot> roots_;
};

}  // namespace myclean::games
