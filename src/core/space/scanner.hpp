#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stop_token>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

// Scanner v1 (space spec, "Safe Scanner Enumeration"): lists a volume or folder in parallel per
// directory and returns it as a folder tree with totals. Sizes are sizes on disk (allocated
// clusters, so compressed and sparse files count what they occupy). Nothing is followed and no
// file content is read: a reparse point is reported as a link of zero size, a cloud placeholder
// is reported with the local size the listing gives and is never opened.
namespace myclean::space {

// One entry of a directory listing, as the operating system reports it.
struct ListedEntry {
  std::filesystem::path name;  // the leaf name
  bool directory = false;
  os::FileAttributes attributes;
  std::uint32_t reparse_tag = 0;  // set when attributes.reparse_point
  Bytes size;                     // logical size; zero for directories
  Bytes size_on_disk;             // allocated size; zero for directories
  WallTime created;
  WallTime modified;  // last write
  FileIdentity identity;
};

// Lists one directory without recursion. Implemented by src/platform/scan; tests use fakes.
class IDirectoryReader {
 public:
  virtual ~IDirectoryReader() = default;
  IDirectoryReader(const IDirectoryReader&) = delete;
  IDirectoryReader& operator=(const IDirectoryReader&) = delete;
  IDirectoryReader(IDirectoryReader&&) = delete;
  IDirectoryReader& operator=(IDirectoryReader&&) = delete;

  // The entries directly inside the absolute directory `path`, without "." and "..". Fails
  // with invalid_argument when `path` is not a directory or is a reparse point, access_denied
  // or not_found as the OS reports. Called from several scanner threads at once.
  [[nodiscard]] virtual Result<std::vector<ListedEntry>> list(
      const std::filesystem::path& path) const = 0;

 protected:
  IDirectoryReader() = default;
};

// A file or a link inside a folder.
struct FileItem {
  std::filesystem::path name;
  // A reparse point that is not followed: a junction, a symbolic link, a mount point or any
  // other reparse folder. Its sizes are zero.
  bool link = false;
  // A cloud file (RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN or OFFLINE): never opened; its size on
  // disk is the local part only.
  bool placeholder = false;
  // Another hard link of a file already counted in this scan (same FileIdentity): listed, but
  // its size is not added to any folder total a second time.
  bool extra_link = false;
  std::uint32_t reparse_tag = 0;  // set for a link
  Bytes size;
  Bytes size_on_disk;
  WallTime created;
  WallTime modified;
  FileIdentity identity;
};

inline constexpr std::size_t no_parent = std::numeric_limits<std::size_t>::max();

struct Folder {
  std::filesystem::path path;        // absolute
  std::size_t parent = no_parent;    // index into ScanResult::folders; no_parent for the root
  std::vector<std::size_t> folders;  // child folders (indices), sorted by name
  std::vector<FileItem> files;       // files and links directly inside, sorted by name
  // Why the folder could not be listed (access denied, removed meanwhile). It then has no
  // entries; the scan goes on.
  std::optional<Error> error;
  // Totals over the folder and everything below it (hard links counted once).
  Bytes size_on_disk;
  Bytes size;
  std::uint64_t file_count = 0;    // files, links excluded
  std::uint64_t folder_count = 0;  // folders below, the folder itself excluded
  Bytes own_size_on_disk;          // files directly inside only
};

struct ScanResult {
  // folders[0] is the scanned root; a parent always has a smaller index than its children.
  std::vector<Folder> folders;
  bool cancelled = false;  // stopped early: the totals cover what was listed until then
  std::uint64_t link_count = 0;
  std::uint64_t placeholder_count = 0;
  std::uint64_t error_count = 0;  // folders that could not be listed

  [[nodiscard]] const Folder& root() const { return folders.front(); }
};

// The absolute path of `file` inside `folder`.
[[nodiscard]] inline std::filesystem::path path_of(const Folder& folder, const FileItem& file) {
  return folder.path / file.name;
}

// A file an analysis of a scan could not read; the analysis goes on without it.
struct ItemError {
  std::filesystem::path path;
  Error error;
};

struct ScanProgress {
  std::uint64_t folders = 0;  // folders listed so far
  std::uint64_t files = 0;    // entries listed so far
  Bytes size_on_disk;         // of the files listed so far
};

struct ScanOptions {
  unsigned workers = 4;                 // directories listed at once; 1..64
  std::uint64_t progress_every = 1000;  // call `progress` after about this many entries
};

// Called from a scanner thread while it holds the scan lock: keep it short.
using ProgressCallback = std::function<void(const ScanProgress&)>;

// Scans the absolute folder `root` (a drive root such as "C:\" scans the volume). Fails when
// `root` itself cannot be listed. Stops early, with `cancelled` set, when `stop` is requested.
[[nodiscard]] Result<ScanResult> scan(const IDirectoryReader& reader,
                                      const std::filesystem::path& root,
                                      const ScanOptions& options = {},
                                      const std::stop_token& stop = {},
                                      const ProgressCallback& progress = {});

}  // namespace myclean::space
