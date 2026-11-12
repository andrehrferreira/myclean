#include "core/space/top.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

namespace myclean::space {

namespace {

struct FileRef {
  const Folder* folder;
  const FileItem* file;
};

// Sorts the `count` largest of `items` to the front, largest first, ties by path, and drops
// the rest. `size` and `path` read an item's size on disk and path.
template <typename Item, typename Size, typename Path>
void keep_largest(std::vector<Item>& items, std::size_t count, Size size, Path path) {
  const auto larger = [&](const Item& left, const Item& right) {
    const Bytes left_size = size(left);
    const Bytes right_size = size(right);
    return left_size != right_size ? left_size > right_size : path(left) < path(right);
  };
  const std::size_t kept = std::min(count, items.size());
  std::ranges::partial_sort(items, items.begin() + static_cast<std::ptrdiff_t>(kept), larger);
  items.resize(kept);
}

}  // namespace

TopItems top_items(const ScanResult& scan, std::size_t count) {
  // References first, so that a volume scan builds full paths only for the entries kept.
  std::vector<FileRef> files;
  std::vector<const Folder*> folders;
  for (const Folder& folder : scan.folders) {
    if (folder.parent != no_parent) {
      folders.push_back(&folder);
    }
    for (const FileItem& file : folder.files) {
      if (!file.link && !file.extra_link) {
        files.push_back(FileRef{.folder = &folder, .file = &file});
      }
    }
  }
  keep_largest(
      files, count, [](const FileRef& ref) { return ref.file->size_on_disk; },
      [](const FileRef& ref) { return path_of(*ref.folder, *ref.file); });
  std::vector<const Folder*> children;
  for (const std::size_t child : scan.root().folders) {
    children.push_back(&scan.folders[child]);
  }
  const auto folder_size = [](const Folder* folder) { return folder->size_on_disk; };
  const auto folder_path = [](const Folder* folder) -> const auto& { return folder->path; };
  keep_largest(folders, count, folder_size, folder_path);
  keep_largest(children, count, folder_size, folder_path);

  const auto row = [](const Folder* folder) {
    return TopFolder{.path = folder->path,
                     .size_on_disk = folder->size_on_disk,
                     .size = folder->size,
                     .file_count = folder->file_count};
  };
  TopItems top{.size_on_disk = scan.root().size_on_disk,
               .own_size_on_disk = scan.root().own_size_on_disk};
  for (const FileRef& ref : files) {
    top.files.push_back(TopFile{.path = path_of(*ref.folder, *ref.file),
                                .size_on_disk = ref.file->size_on_disk,
                                .size = ref.file->size,
                                .modified = ref.file->modified});
  }
  std::ranges::transform(folders, std::back_inserter(top.folders), row);
  std::ranges::transform(children, std::back_inserter(top.children), row);
  return top;
}

}  // namespace myclean::space
