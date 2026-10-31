#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include "core/health/analysis.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"
#include "space/space_fakes.hpp"

namespace myclean::scanall::testing {

using namespace std::chrono_literals;

// The time every scan-all test runs at.
inline const WallTime now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};

inline DriveInfo drive(std::wstring root, placement::DriveClass drive_class,
                       health::Status status = health::Status::good,
                       Bytes free = Bytes{std::uint64_t{500} << 30U}) {
  return DriveInfo{.root = std::filesystem::path{std::move(root)},
                   .drive_class = drive_class,
                   .health = status,
                   .health_note = status == health::Status::good ? "" : "pending sector",
                   .size = Bytes{std::uint64_t{1000} << 30U},
                   .free = free};
}

// A file of `size` bytes on disk, created and last written at `time`, with file id `id` on
// volume `volume` (the same identity twice is one file with two hard links).
inline space::ListedEntry file(const std::string& name, Bytes size, WallTime time, std::uint8_t id,
                               std::uint64_t volume = 1) {
  space::ListedEntry entry = space::testing::file(name, size.count(), id);
  entry.identity.volume_serial = volume;
  entry.created = time;
  entry.modified = time;
  return entry;
}

inline space::ListedEntry folder(const std::string& name) {
  return space::testing::folder(name);
}

}  // namespace myclean::scanall::testing
