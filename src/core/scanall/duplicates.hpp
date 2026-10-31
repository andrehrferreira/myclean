#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <vector>

#include "core/model/units.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/scanner.hpp"

// Duplicates across every drive (scan-all spec, "Cross-Drive Duplicates"), with the copy to keep.
// Read-only: content is only hashed.
namespace myclean::scanall {

struct DuplicateOptions {
  Bytes min_size{std::uint64_t{1} << 20U};  // smaller files are not compared
  Bytes budget{std::uint64_t{8} << 30U};    // file content hashed, at most
  // Libraries and project roots: a copy inside one is kept before others.
  std::vector<std::filesystem::path> preferred;
  // Downloads, TEMP and the like: a copy inside one is kept only when every copy is.
  std::vector<std::filesystem::path> transient;
};

struct DuplicateRun {
  std::vector<Finding> findings;
  std::size_t groups = 0;   // sets of identical files found
  Bytes hashed;             // content read for hashes
  std::size_t skipped = 0;  // hashes left out because they did not fit the budget
  bool cancelled = false;   // stopped by `stop`
  std::size_t errors = 0;   // files that could not be hashed
};

// The identical files of `trees` (one per drive) by space::find_duplicates (size, then a hash of
// the first 64 KiB, then of the whole content; links, cloud placeholders and further hard links
// of a file are skipped), within `budget` bytes hashed: largest sizes first, a hash that does not
// fit what is left of the budget is skipped (its file is not compared) and the search stops when
// not even a 64 KiB head hash fits. In each set one copy is kept, chosen in
// this order: on a healthy drive, inside a `preferred` folder, outside the `transient` folders,
// the newest, the first by path. The other copies give one deduplicate finding per drive they
// are on, whose plan quarantines them (quarantine_items, `keep` names the kept copy).
[[nodiscard]] DuplicateRun find_duplicates(std::span<const space::ScanResult> trees,
                                           std::span<const DriveInfo> drives,
                                           const space::IContentHasher& hasher,
                                           const DuplicateOptions& options = {},
                                           const std::stop_token& stop = {});

}  // namespace myclean::scanall
