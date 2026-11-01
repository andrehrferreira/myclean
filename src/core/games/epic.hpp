#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"

// Epic Games Launcher install manifests: %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\
// *.item, one JSON file per installed app (spike X5 §2).
namespace myclean::games {

struct EpicManifest {
  std::string display_name;
  std::filesystem::path install_location;
  std::string launch_executable;        // relative to install_location; empty for a plugin
  std::vector<std::string> categories;  // AppCategories: "games", "engines/ue5", ...
};

// Reads DisplayName, InstallLocation, LaunchExecutable and AppCategories; a missing field is
// left empty. Fails with invalid_data when the text is not a JSON object or a field has the
// wrong type.
[[nodiscard]] Result<EpicManifest> parse_epic_manifest(std::string_view text);

// True when the manifest installs a game: AppCategories contains "games" and neither "engines"
// nor "plugins" (also as the first part of "engines/ue5"), and LaunchExecutable is set. Unreal
// Engine, its plugins and Fab content are not games: their tools (UnrealEditor,
// ShaderCompileWorker) belong to class build.
[[nodiscard]] bool is_epic_game(const EpicManifest& manifest);

}  // namespace myclean::games
