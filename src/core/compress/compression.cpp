#include "core/compress/compression.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/clean/patterns.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::compress {

namespace {

constexpr auto algorithm_names =
    std::to_array<std::string_view>({"xpress4k", "xpress8k", "xpress16k", "lzx", "lznt1"});
constexpr auto exclusion_names = std::to_array<std::string_view>(
    {"none", "link", "placeholder", "extra_link", "small", "compressed", "media", "disk_image",
     "database", "system_file", "recent", "windows", "incompressible", "pack_excluded"});
constexpr auto category_names =
    std::to_array<std::string_view>({"toolchain", "engine", "game", "program", "other"});

// Formats that are compressed already: pictures, audio, video, archives and packages.
constexpr auto media_extensions = std::to_array<std::string_view>(
    {".jpg",   ".jpeg", ".png",  ".gif", ".webp",  ".heic", ".heif",       ".avif", ".jxl",
     ".mp3",   ".aac",  ".m4a",  ".ogg", ".opus",  ".flac", ".wma",        ".wem",  ".xwma",
     ".mp4",   ".m4v",  ".mkv",  ".mov", ".avi",   ".webm", ".wmv",        ".flv",  ".bik",
     ".bk2",   ".usm",  ".zip",  ".7z",  ".rar",   ".gz",   ".tgz",        ".bz2",  ".xz",
     ".zst",   ".lz4",  ".lzma", ".cab", ".msi",   ".msix", ".msixbundle", ".appx", ".appxbundle",
     ".nupkg", ".jar",  ".apk",  ".whl", ".crate", ".wim",  ".esd",        ".docx"});
constexpr auto disk_image_extensions = std::to_array<std::string_view>(
    {".vhd", ".vhdx", ".avhd", ".avhdx", ".vmdk", ".vdi", ".qcow2"});
constexpr auto database_extensions = std::to_array<std::string_view>(
    {".db",         ".db-wal", ".db-shm", ".db-journal", ".sqlite", ".sqlite3", ".sqlite-wal",
     ".sqlite-shm", ".wal",    ".mdf",    ".ldf",        ".ndf",    ".edb",     ".jrs",
     ".log",        ".etl",    ".ldb",    ".mdb",        ".accdb",  ".journal"});
constexpr auto system_extensions = std::to_array<std::string_view>({".sys"});

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& list, std::string_view extension) {
  return std::ranges::find(list, extension) != list.end();
}

// "\<path lower case>\" with backslashes, so that markers match whole components.
std::string marked(const std::filesystem::path& path) {
  std::string text = space::lower_ascii(
      space::utf8(std::filesystem::path{path}.lexically_normal().make_preferred()));
  if (!text.ends_with('\\')) {
    text += '\\';
  }
  return '\\' + text;
}

template <std::size_t N>
bool contains_any(std::string_view text, const std::array<std::string_view, N>& markers) {
  return std::ranges::any_of(markers, [&](std::string_view marker) {
    return text.find(marker) != std::string_view::npos;
  });
}

constexpr auto engine_markers = std::to_array<std::string_view>(
    {R"(\epic games\ue_)", R"(\unreal engine\)", R"(\unity\hub\editor\)", R"(\unity\editor\)",
     R"(\unity editor\)", R"(\godot)"});
constexpr auto game_markers = std::to_array<std::string_view>(
    {R"(\steamapps\common\)", R"(\epic games\)", R"(\gog games\)", R"(\gog galaxy\games\)",
     R"(\xboxgames\)", R"(\riot games\)", R"(\ubisoft game launcher\games\)", R"(\ea games\)",
     R"(\battle.net\games\)"});
constexpr auto toolchain_markers = std::to_array<std::string_view>({R"(\.rustup\)",
                                                                    R"(\.cargo\)",
                                                                    R"(\microsoft visual studio\)",
                                                                    R"(\windows kits\)",
                                                                    R"(\android\sdk\)",
                                                                    R"(\.gradle\)",
                                                                    R"(\.m2\)",
                                                                    R"(\.dotnet\)",
                                                                    R"(\program files\dotnet\)",
                                                                    R"(\llvm\)",
                                                                    R"(\nodejs\)",
                                                                    R"(\.nuget\packages\)",
                                                                    R"(\.vscode\extensions\)",
                                                                    R"(\msys64\)",
                                                                    R"(\mingw64\)",
                                                                    R"(\.platformio\)",
                                                                    R"(\flutter\)",
                                                                    R"(\.espressif\)",
                                                                    R"(\java\jdk)",
                                                                    R"(\.jdks\)",
                                                                    R"(\go\pkg\mod\)",
                                                                    R"(\microsoft sdks\)"});
constexpr auto program_markers =
    std::to_array<std::string_view>({R"(\program files\)", R"(\program files (x86)\)"});

// True when a file or folder of `path` below `rules.excluded_below` has an excluded name.
bool excluded_by_name(const std::filesystem::path& path, const Rules& rules) {
  if (rules.excluded_names.empty() || !is_within(path, rules.excluded_below)) {
    return false;
  }
  const std::filesystem::path inside = path.lexically_relative(rules.excluded_below);
  return std::ranges::any_of(inside, [&rules](const std::filesystem::path& name) {
    return std::ranges::any_of(rules.excluded_names, [&name](const std::string& pattern) {
      return clean::wildcard_match(
          name.native(),
          std::filesystem::path{std::u8string(pattern.begin(), pattern.end())}.native());
    });
  });
}

}  // namespace

std::string_view to_string(Algorithm algorithm) {
  return detail::enum_name(algorithm_names, algorithm);
}

std::optional<Algorithm> parse_algorithm(std::string_view text) {
  return detail::enum_from_name<Algorithm>(algorithm_names, text);
}

std::uint64_t chunk_bytes(Algorithm algorithm) {
  constexpr std::uint64_t kib = 1024;
  switch (algorithm) {
    case Algorithm::xpress8k:
      return 8 * kib;
    case Algorithm::xpress16k:
      return 16 * kib;
    case Algorithm::lzx:
      return 32 * kib;
    case Algorithm::xpress4k:
    case Algorithm::lznt1:
      break;
  }
  return 4 * kib;
}

std::string_view to_string(Exclusion exclusion) {
  return detail::enum_name(exclusion_names, exclusion);
}

std::string_view to_string(Category category) {
  return detail::enum_name(category_names, category);
}

Category categorize(const std::filesystem::path& folder) {
  const std::string text = marked(folder);
  if (contains_any(text, engine_markers)) {
    return Category::engine;
  }
  if (contains_any(text, game_markers)) {
    return Category::game;
  }
  if (contains_any(text, toolchain_markers)) {
    return Category::toolchain;
  }
  if (contains_any(text, program_markers)) {
    return Category::program;
  }
  return Category::other;
}

bool is_within(const std::filesystem::path& path, const std::filesystem::path& folder) {
  return marked(path).starts_with(marked(folder));
}

Exclusion exclusion(const space::Folder& folder, const space::FileItem& file, const Rules& rules,
                    WallTime now) {
  if (file.link) {
    return Exclusion::link;
  }
  if (file.placeholder) {
    return Exclusion::placeholder;
  }
  if (file.extra_link) {
    return Exclusion::extra_link;
  }
  if (file.size <= rules.cluster) {
    return Exclusion::small;
  }
  if (file.size_on_disk < file.size) {
    return Exclusion::compressed;
  }
  const std::string extension = space::lower_extension(file.name);
  if (listed(media_extensions, extension)) {
    return Exclusion::media;
  }
  if (listed(disk_image_extensions, extension)) {
    return Exclusion::disk_image;
  }
  if (listed(database_extensions, extension)) {
    return Exclusion::database;
  }
  if (listed(system_extensions, extension)) {
    return Exclusion::system_file;
  }
  if (!rules.windows.empty() && is_within(folder.path, rules.windows)) {
    return Exclusion::windows;
  }
  if (std::ranges::find(rules.incompressible, extension) != rules.incompressible.end()) {
    return Exclusion::incompressible;
  }
  if (excluded_by_name(folder.path / file.name, rules)) {
    return Exclusion::pack_excluded;
  }
  if (file.modified > now - rules.recent) {
    return Exclusion::recent;
  }
  return Exclusion::none;
}

}  // namespace myclean::compress
