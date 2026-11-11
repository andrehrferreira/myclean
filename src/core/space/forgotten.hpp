#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Large forgotten files (space spec, SPC-5): big files of the owner's folders that nobody has
// written for a long time. The age is that of the newer of the creation and last-write times, as
// for old downloads; last-access time is never used. Read-only: a report, never a plan.
namespace myclean::space {

inline constexpr Bytes default_forgotten_size{512ULL * 1024 * 1024};  // 512 MiB
inline constexpr std::chrono::days default_forgotten_age{180};

struct ForgottenFile {
  std::filesystem::path path;
  Bytes size;
  Bytes size_on_disk;
  WallTime created;
  WallTime modified;
  std::int64_t idle_days = 0;  // whole days since the newer of created and modified
};

// The files of `scans` at least `min_size` on disk and idle for more than `idle` at `now`,
// largest on disk first. Links, extra hard links and cloud placeholders are skipped: a
// placeholder takes no room on this disk.
[[nodiscard]] std::vector<ForgottenFile> forgotten_files(
    std::span<const ScanResult> scans, WallTime now, Bytes min_size = default_forgotten_size,
    std::chrono::days idle = default_forgotten_age);

}  // namespace myclean::space
