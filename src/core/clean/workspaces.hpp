#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"

// data/workspaces.toml: where the developer-artifact scanner looks for projects.
//
//   roots     = ["F:\\", "W:\\"]             # required: absolute folders to walk
//   skip      = [".git", "node_modules"]     # optional: folder names never entered (any case)
//   max_depth = 8                            # optional, 1..64: folder levels below a root
namespace myclean::clean {

struct Workspaces {
  std::vector<std::filesystem::path> roots;
  std::vector<std::wstring> skip;
  int max_depth = 8;
};

// Validates the whole file; one Errc::invalid_data error names the file and the field.
[[nodiscard]] Result<Workspaces> parse_workspace_file(std::string_view text,
                                                      const std::filesystem::path& file);

}  // namespace myclean::clean
