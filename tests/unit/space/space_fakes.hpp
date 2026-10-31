#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::space::testing {

inline constexpr std::uint32_t tag_mount_point = 0xA0000003U;  // junction
inline constexpr std::uint32_t tag_symlink = 0xA000000CU;
inline constexpr std::uint32_t tag_cloud = 0x9000001AU;

inline FileIdentity identity(std::uint8_t id, std::uint64_t volume = 1) {
  FileIdentity result{.volume_serial = volume};
  result.file_id[0] = id;
  return result;
}

inline ListedEntry file(const std::string& name, std::uint64_t size_on_disk, std::uint8_t id = 0) {
  return ListedEntry{.name = name,
                     .size = Bytes{size_on_disk},
                     .size_on_disk = Bytes{size_on_disk},
                     .identity = identity(id)};
}

inline ListedEntry folder(const std::string& name) {
  return ListedEntry{.name = name, .directory = true};
}

inline ListedEntry junction(const std::string& name) {
  return ListedEntry{.name = name,
                     .directory = true,
                     .attributes = {.reparse_point = true},
                     .reparse_tag = tag_mount_point};
}

// An in-memory directory tree. Folders not listed in `listings` fail with not_found.
class FakeDirectoryReader final : public IDirectoryReader {
 public:
  std::map<std::filesystem::path, std::vector<ListedEntry>> listings;
  std::map<std::filesystem::path, Error> errors;
  std::function<void()> on_list;  // runs at the start of every list() call

  [[nodiscard]] Result<std::vector<ListedEntry>> list(
      const std::filesystem::path& path) const override {
    if (on_list) {
      on_list();
    }
    {
      const std::scoped_lock lock{mutex};
      listed.insert(path);
    }
    if (const auto error = errors.find(path); error != errors.end()) {
      return tl::unexpected(error->second);
    }
    const auto listing = listings.find(path);
    if (listing == listings.end()) {
      return fail(Errc::not_found, "no such folder");
    }
    return listing->second;
  }

  [[nodiscard]] bool was_listed(const std::filesystem::path& path) const {
    const std::scoped_lock lock{mutex};
    return listed.contains(path);
  }

  mutable std::mutex mutex;                        // guards `listed`
  mutable std::set<std::filesystem::path> listed;  // every folder list() was called for
};

// Scans `reader` from `root` and fails the test when the scan fails.
inline ScanResult scan_ok(const IDirectoryReader& reader, const std::filesystem::path& root,
                          const ScanOptions& options = {}) {
  Result<ScanResult> result = scan(reader, root, options);
  if (!result) {
    throw std::runtime_error("scan failed: " + result.error().message);
  }
  return std::move(*result);
}

// The folder of `scan` at `path`; throws when there is none.
inline const Folder& folder_at(const ScanResult& scan, const std::filesystem::path& path) {
  for (const Folder& folder : scan.folders) {
    if (folder.path == path) {
      return folder;
    }
  }
  throw std::runtime_error("no folder " + utf8(path));
}

}  // namespace myclean::space::testing
