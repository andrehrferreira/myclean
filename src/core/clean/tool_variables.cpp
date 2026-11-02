#include "core/clean/tool_variables.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

}  // namespace

bool is_tool_variable(std::wstring_view name) {
  return same_name(name, steam_library_variable);
}

std::optional<std::vector<std::wstring>> tool_variable(std::wstring_view name,
                                                       const ToolFiles& files) {
  if (same_name(name, steam_library_variable)) {
    return steam_libraries(files);
  }
  return std::nullopt;
}

}  // namespace myclean::clean
