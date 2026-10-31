#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Exact duplicates (space spec, "Exact Duplicates"): files grouped by size, then by a hash of
// their first 64 KB, then by a hash of their whole content. Hard links of one file are the same
// file, not duplicates. Read-only.
namespace myclean::space {

// A 256-bit content hash.
using Digest = std::array<std::uint8_t, 32>;

// Hashes file content. Implemented by src/platform/scan (SHA-256 through BCrypt).
class IContentHasher {
 public:
  virtual ~IContentHasher() = default;
  IContentHasher(const IContentHasher&) = delete;
  IContentHasher& operator=(const IContentHasher&) = delete;
  IContentHasher(IContentHasher&&) = delete;
  IContentHasher& operator=(IContentHasher&&) = delete;

  // The hash of the first `limit` bytes of `file`, or of all of it when `limit` is nullopt.
  // Fails with refused for a link or a cloud placeholder, whose content is never read.
  [[nodiscard]] virtual Result<Digest> hash(const std::filesystem::path& file,
                                            std::optional<Bytes> limit) const = 0;

 protected:
  IContentHasher() = default;
};

inline constexpr Bytes head_hash_bytes{std::uint64_t{64} * 1024};

struct DuplicateOptions {
  Bytes min_size{1};                    // smaller files are ignored; empty files always are
  std::vector<std::string> extensions;  // lower case with the dot (".jpg"); empty: every file
};

struct DuplicateFile {
  std::filesystem::path path;
  Bytes size_on_disk;
  WallTime modified;
  FileIdentity identity;
};

struct DuplicateGroup {
  Bytes size;                        // of each file
  Digest digest;                     // of the whole content
  Bytes reclaimable;                 // freed by keeping only the copy that is largest on disk
  std::vector<DuplicateFile> files;  // two or more, by path
};

struct DuplicateReport {
  std::vector<DuplicateGroup> groups;  // most reclaimable first
  std::vector<ItemError> errors;       // files that could not be hashed
  bool cancelled = false;              // stopped early: the groups found so far
};

// The exact duplicates among the files of `scans` (one per drive or folder; overlapping scans
// are fine). A file whose size no other file has is never read. Links, cloud placeholders and
// further hard links of a file are skipped.
[[nodiscard]] DuplicateReport find_duplicates(std::span<const ScanResult> scans,
                                              const IContentHasher& hasher,
                                              const DuplicateOptions& options = {},
                                              const std::stop_token& stop = {});

// Lower-case hexadecimal text of a digest.
[[nodiscard]] std::string to_hex(const Digest& digest);

}  // namespace myclean::space
