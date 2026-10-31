#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/index/mft.hpp"
#include "core/index/usn.hpp"
#include "core/index/volume.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

namespace myclean::index {

// The totals of one folder.
struct FolderSize {
  std::filesystem::path path;
  Bytes size_on_disk;
  Bytes size;

  friend bool operator==(const FolderSize&, const FolderSize&) = default;
};

// The whole-volume index (ntfs-index spec): every file and folder of one NTFS volume as read
// from its $MFT, with each folder's total size on disk kept current by the USN journal.
//
// Sizes follow scanner v1 (space::scan) so that either can feed the map and the analytics:
// sizes on disk are allocated clusters (compressed and sparse streams count what they occupy);
// a reparse point that names another entry (junction, symbolic link, mount point) or any reparse
// folder is a link of zero size and is never followed; a cloud placeholder counts its local
// part only; a file with several hard links is counted once, at the link that comes first in a
// depth-first walk by name. Unlike a directory listing the index also shows the NTFS metadata
// files in the root ($MFT, $LogFile, $Bitmap, ...) and counts named streams that occupy
// clusters (WofCompressedData of a Windows-compressed file, for example).
//
// Compact: one 48-byte node per name, the UTF-8 name and a 4-byte slot per $MFT record.
// Not thread-safe; the owner serializes access.
class VolumeIndex {
 public:
  // An empty index of the volume whose root folder is `root` ("C:\").
  VolumeIndex(std::filesystem::path root, std::uint64_t volume_serial);

  // Building: add every in-use FILE record segment of the $MFT (base and extension segments,
  // in any order), then finish(), which links the names into a tree and totals it. Names whose
  // parent folder is missing or not reachable from the root are dropped and counted as
  // unparented. Fails when the root folder (record 5) is missing.
  void add(const RecordSegment& segment);
  [[nodiscard]] Result<void> finish();

  // Brings the index up to date with journal `records` (in USN order): a deleted file is
  // removed, every other file named is re-read from `volume` (its current names, sizes and
  // attributes) and re-linked, moving its totals to the new parent folders. A folder created
  // in the same batch is read before its contents.
  [[nodiscard]] Result<void> apply(std::span<const UsnRecord> records, const INtfsVolume& volume);

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }
  [[nodiscard]] Bytes size_on_disk() const;  // of the whole volume
  [[nodiscard]] std::size_t node_count() const { return nodes_.size(); }
  // Nodes left behind by deletions and renames; a rebuild drops them.
  [[nodiscard]] std::size_t dead_count() const { return dead_; }
  [[nodiscard]] std::uint64_t unparented_count() const { return unparented_; }
  // Estimated heap bytes held by the index.
  [[nodiscard]] std::size_t memory_bytes() const;

  // Every folder whose size on disk is at least `min_size_on_disk`, sorted by path.
  [[nodiscard]] std::vector<FolderSize> folder_sizes(Bytes min_size_on_disk) const;

  // The tree below `folder` (the root or any folder of the volume, matched case-insensitively
  // for ASCII letters) in the shape scanner v1 returns, with the same totals a scan of that
  // folder gives. `with_files` false leaves every Folder::files empty (totals still count
  // them), for a folder-level view of a whole volume. Fails with not_found for a path that is
  // not in the index and invalid_argument for one that is not a folder.
  [[nodiscard]] Result<space::ScanResult> to_scan_result(const std::filesystem::path& folder,
                                                         bool with_files = true) const;

 private:
  struct Node {
    std::uint64_t size = 0;          // a file's logical size; a folder's total; a link's tag
    std::uint64_t size_on_disk = 0;  // a file's allocation; a folder's total
    std::uint32_t parent = 0;        // node index
    std::uint32_t name_offset = 0;   // into names_
    std::uint32_t record = 0;
    std::uint32_t next_link = 0;  // the next node (hard link) of the same record
    std::uint32_t created = 0;    // Unix seconds
    std::uint32_t modified = 0;
    std::uint16_t name_length = 0;
    std::uint16_t sequence = 0;
    std::uint16_t flags = 0;
  };

  // A file record's attributes merged from its segments.
  struct Facts {
    std::uint64_t size = 0;
    std::uint64_t size_on_disk = 0;
    std::uint32_t attributes = 0;
    std::uint32_t reparse_tag = 0;
    std::uint32_t created = 0;
    std::uint32_t modified = 0;
    std::uint16_t sequence = 0;
    bool in_use = false;
    bool directory = false;
  };

  struct FileState {
    Facts facts;
    std::vector<FileName> names;
  };

  // The children of every live node, by parent (compressed rows).
  struct Children {
    std::vector<std::uint32_t> offsets;  // node -> first child in `nodes`; one more at the end
    std::vector<std::uint32_t> nodes;
  };

  // The files of each folder of a ScanResult under construction, sorted by name.
  using FileLists = std::vector<std::vector<std::uint32_t>>;

  static void fold(Facts& facts, const RecordSegment& segment);
  [[nodiscard]] static std::uint16_t flags_of(const Facts& facts);
  [[nodiscard]] static Result<std::optional<FileState>> read_file(const INtfsVolume& volume,
                                                                  std::uint64_t record);

  // The offset of `name` added to names_; nullopt when the pool is full (4 GiB).
  [[nodiscard]] std::optional<std::uint32_t> append_name(std::string_view name);
  [[nodiscard]] std::uint32_t append_node(std::uint32_t record, std::string_view name);
  void set_facts(std::uint32_t node, const Facts& facts);
  [[nodiscard]] std::string_view name_of(std::uint32_t node) const;
  [[nodiscard]] std::filesystem::path path_of(std::uint32_t node) const;
  [[nodiscard]] std::vector<std::uint32_t> nodes_of(std::uint32_t record) const;
  [[nodiscard]] bool live(std::uint32_t node) const;
  [[nodiscard]] bool is_folder(std::uint32_t node) const;
  [[nodiscard]] bool is_link(std::uint32_t node) const;
  [[nodiscard]] bool counted(std::uint32_t node) const;
  [[nodiscard]] bool several_links(std::uint32_t node) const;
  // True when `a` comes before `b` in a depth-first walk by name (files before subfolders).
  [[nodiscard]] bool before(std::uint32_t a, std::uint32_t b) const;
  void map(std::uint32_t node);
  void unmap(std::uint32_t node);
  void choose_counted(std::uint32_t record);
  // Adds (or removes) a node's sizes to the totals of every live folder above it.
  void credit(std::uint32_t node, bool add);
  void detach(std::uint32_t node);
  void remove(std::uint64_t reference);
  [[nodiscard]] Result<void> refresh(std::uint64_t reference, const INtfsVolume& volume,
                                     unsigned depth);
  [[nodiscard]] Result<void> upsert(std::uint32_t record, const FileState& state,
                                    const INtfsVolume& volume, unsigned depth);
  [[nodiscard]] Result<void> move_folder(std::uint32_t node, const FileState& state,
                                         const INtfsVolume& volume, unsigned depth);
  [[nodiscard]] Result<void> relink(std::uint32_t record, const FileState& state,
                                    const INtfsVolume& volume, unsigned depth);
  [[nodiscard]] Result<std::uint32_t> resolve_parent(std::uint64_t reference,
                                                     const INtfsVolume& volume, unsigned depth);
  [[nodiscard]] std::uint32_t folder_node(std::uint64_t reference) const;
  void link_parents();
  [[nodiscard]] Children children() const;
  [[nodiscard]] Result<std::uint32_t> find(const std::filesystem::path& folder,
                                           const Children& children) const;
  // Fills `result` with the folders below `start`, breadth first, and returns their files.
  [[nodiscard]] FileLists list_tree(std::uint32_t start, const Children& children,
                                    space::ScanResult& result) const;
  // The nodes that are extra hard links in a depth-first walk by name of `result`.
  [[nodiscard]] std::vector<bool> extra_links(const space::ScanResult& result,
                                              const FileLists& files) const;
  [[nodiscard]] space::FileItem file_item(std::uint32_t node, bool extra_link) const;

  std::filesystem::path root_;
  std::uint64_t serial_ = 0;
  std::vector<Node> nodes_;
  std::string names_;
  std::vector<std::uint32_t> first_node_;  // record -> its first live node
  std::uint32_t root_node_ = 0;
  std::size_t dead_ = 0;
  std::uint64_t unparented_ = 0;
  // Building only.
  std::vector<Facts> facts_;                // by record
  std::vector<std::uint64_t> parent_refs_;  // by node
};

}  // namespace myclean::index
