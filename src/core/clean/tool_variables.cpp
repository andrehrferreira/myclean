#include "core/clean/tool_variables.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/clean/probes.hpp"
#include "core/games/paths.hpp"
#include "core/games/steam.hpp"
#include "core/model/result.hpp"

namespace myclean::clean {

namespace {

namespace fs = std::filesystem;

bool same_name(std::wstring_view left, std::wstring_view right) {
  const auto fold = [](wchar_t character) {
    return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a')
                                                  : character;
  };
  return std::ranges::equal(left, right, {}, fold, fold);
}

// `path` as a value: absolute, '\'-separated, without a trailing separator; nullopt when it is
// not absolute.
std::optional<std::wstring> folder_value(const fs::path& path) {
  fs::path folder = path.lexically_normal().make_preferred();
  if (!folder.has_root_name() || !folder.has_root_directory()) {
    return std::nullopt;
  }
  if (folder.has_relative_path() && folder.filename().empty()) {
    folder = folder.parent_path();
  }
  return folder.native();
}

std::vector<std::wstring> steam_libraries(const ToolFiles& files) {
  if (!files.steam || !files.read) {
    return {};
  }
  const Result<std::string> text = files.read(games::library_folders_file(*files.steam));
  if (!text) {
    return {};
  }
  const Result<std::vector<fs::path>> libraries = games::parse_library_folders(*text);
  if (!libraries) {
    return {};
  }
  std::vector<std::wstring> values;
  for (const fs::path& library : *libraries) {
    if (std::optional<std::wstring> value = folder_value(library)) {
      values.push_back(std::move(*value));
    }
  }
  return values;
}

// CustomWslDistroDir of a Docker Desktop settings file (settings-store.json spells the key so, the
// legacy settings.json customWslDistroDir): the folder, nullopt when it names none, or an error.
Result<std::optional<std::wstring>> custom_wsl_folder(const std::string& text) {
  const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return fail(Errc::invalid_data, "not a JSON object");
  }
  for (const auto& item : json.items()) {
    if (!same_name(games::path_from_utf8(item.key()).native(), L"CustomWslDistroDir")) {
      continue;
    }
    if (!item.value().is_string()) {
      return fail(Errc::invalid_data, "CustomWslDistroDir is not a string");
    }
    const std::string folder = item.value().get<std::string>();
    if (folder.empty()) {
      return std::optional<std::wstring>{};
    }
    std::optional<std::wstring> absolute = folder_value(games::path_from_utf8(folder));
    if (!absolute) {
      return fail(Errc::invalid_data, "CustomWslDistroDir is not an absolute path");
    }
    return absolute;
  }
  return std::optional<std::wstring>{};
}

std::vector<std::wstring> docker_wsl_folders(const IEnvironment& environment,
                                             const ToolFiles& files) {
  const std::optional<std::wstring> app_data = environment.variable(L"APPDATA");
  if (!app_data || !files.read) {
    return {};
  }
  std::vector<std::wstring> values;
  bool read = false;
  for (const wchar_t* name : std::array{L"settings-store.json", L"settings.json"}) {
    const Result<std::string> text = files.read(fs::path{*app_data} / L"Docker" / name);
    if (!text && text.error().code == Errc::not_found) {
      continue;
    }
    const Result<std::optional<std::wstring>> folder =
        text ? custom_wsl_folder(*text)
             : Result<std::optional<std::wstring>>{tl::unexpected(text.error())};
    if (!folder) {
      return {};  // a setting that cannot be read may name the folder in use
    }
    read = true;
    if (*folder) {
      values.push_back(**folder);
    }
  }
  const std::optional<std::wstring> local_app_data = environment.variable(L"LOCALAPPDATA");
  if (!read || !local_app_data) {
    return {};
  }
  if (std::optional<std::wstring> fallback =
          folder_value(fs::path{*local_app_data} / L"Docker" / L"wsl")) {
    values.push_back(std::move(*fallback));
  }
  return values;
}

}  // namespace

bool is_tool_variable(std::wstring_view name) {
  return same_name(name, steam_library_variable) || same_name(name, docker_wsl_data_variable);
}

std::optional<std::vector<std::wstring>> tool_variable(std::wstring_view name,
                                                       const IEnvironment& environment,
                                                       const ToolFiles& files) {
  if (same_name(name, steam_library_variable)) {
    return steam_libraries(files);
  }
  if (same_name(name, docker_wsl_data_variable)) {
    return docker_wsl_folders(environment, files);
  }
  return std::nullopt;
}

}  // namespace myclean::clean
