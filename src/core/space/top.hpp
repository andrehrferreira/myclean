#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Top-N files and folders of a scan (space spec, "Top-N Files and Folders"). Read-only.
namespace myclean::space {

inline constexpr std::size_t default_top_count = 20;

struct TopFile {
  std::filesystem::path path;
  Bytes size_on_disk;
  Bytes size;
  WallTime modified;
};

struct TopFolder {
  std::filesystem::path path;
  Bytes size_on_disk;  // the folder and everything below it
  Bytes size;
  std::uint64_t file_count = 0;
};

struct TopItems {
  std::vector<TopFile> files;      // largest first
  std::vector<TopFolder> folders;  // largest first
};

// The `count` largest files and folders below the scan root, by size on disk; ties by path.
// The root itself is not listed (every folder of the scan is inside it), nor are links or the
// extra hard links of a file already listed.
[[nodiscard]] TopItems top_items(const ScanResult& scan, std::size_t count = default_top_count);

}  // namespace myclean::space
