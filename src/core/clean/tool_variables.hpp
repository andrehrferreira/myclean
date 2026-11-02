#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/model/result.hpp"

// Variables whose values a tool's own files hold, not the environment block: the folders rule
// targets reach through them (IEnvironment::values).
namespace myclean::clean {

inline constexpr std::wstring_view steam_library_variable = L"STEAM_LIBRARY";
inline constexpr std::wstring_view docker_wsl_data_variable = L"DOCKER_WSL_DATA";

// What tool_variable() reads.
struct ToolFiles {
  // Steam's folder (the SteamPath of the owner's Software\Valve\Steam); nullopt: not installed.
  std::optional<std::filesystem::path> steam;
  // The text of a small file; fails with not_found when it does not exist. Never follows a link
  // or recalls a cloud file.
  std::function<Result<std::string>(const std::filesystem::path&)> read;
};

// The values of `name` when it is one of these variables (case-insensitive), nullopt for any
// other. Each value is an absolute folder, '\'-separated, without a trailing separator.
// `environment` is the owner's: %APPDATA% and %LOCALAPPDATA% are read from it.
//   STEAM_LIBRARY    every library folder that <steam>\steamapps\libraryfolders.vdf lists
//                    (games::parse_library_folders), Steam's own folder among them; none when
//                    Steam is not installed or the file cannot be read or parsed.
//   DOCKER_WSL_DATA  every folder Docker Desktop's settings name for its WSL data (its disk
//                    image location): the CustomWslDistroDir of settings-store.json and of the
//                    legacy settings.json in %APPDATA%\Docker, and the default
//                    %LOCALAPPDATA%\Docker\wsl. None when neither file exists, or
//                    when one that exists cannot be read or parsed or names a folder that is not
//                    absolute: then which folder Docker uses is unknown.
[[nodiscard]] std::optional<std::vector<std::wstring>> tool_variable(
    std::wstring_view name, const IEnvironment& environment, const ToolFiles& files);

// True when tool_variable() reads `name` from a tool's files.
[[nodiscard]] bool is_tool_variable(std::wstring_view name);

}  // namespace myclean::clean
