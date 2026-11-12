#include "core/tray_model/space_map.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/units.hpp"
#include "core/space/treemap.hpp"
#include "core/tray_model/cleaning.hpp"

namespace myclean::tray_model {

namespace {

// The last component of a Windows path: "owner" of "C:\Users\owner".
std::string leaf_name(std::string_view path) {
  while (path.size() > 1 && path.back() == '\\') {
    path.remove_suffix(1);
  }
  const std::size_t slash = path.rfind('\\');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

}  // namespace

std::vector<TreemapCell> treemap_cells(const SpaceReply& reply, const space::Rect& bounds) {
  std::vector<TreemapCell> cells;
  Bytes listed = reply.top_own_size;
  for (const TopFolderRow& child : reply.top_children) {
    cells.push_back(TreemapCell{.kind = TreemapCell::Kind::folder,
                                .folder = child.id,
                                .label = leaf_name(child.path),
                                .size_on_disk = child.size_on_disk});
    listed += child.size_on_disk;
  }
  cells.push_back(TreemapCell{
      .kind = TreemapCell::Kind::files, .label = "files here", .size_on_disk = reply.top_own_size});
  if (reply.top_size > listed) {
    cells.push_back(TreemapCell{.kind = TreemapCell::Kind::other,
                                .label = "other folders",
                                .size_on_disk = Bytes{reply.top_size.count() - listed.count()}});
  }
  std::erase_if(cells, [](const TreemapCell& cell) { return cell.size_on_disk.count() == 0; });
  std::vector<double> sizes;
  sizes.reserve(cells.size());
  for (const TreemapCell& cell : cells) {
    sizes.push_back(static_cast<double>(cell.size_on_disk.count()));
  }
  const std::vector<space::Rect> rects = space::squarify(sizes, bounds);
  for (std::size_t index = 0; index < cells.size(); ++index) {
    cells[index].rect = rects[index];
  }
  return cells;
}

const TreemapCell* cell_at(const std::vector<TreemapCell>& cells, double x, double y) {
  for (const TreemapCell& cell : cells) {
    const space::Rect& rect = cell.rect;
    if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
      return &cell;
    }
  }
  return nullptr;
}

void FolderTrail::open(std::int64_t id) {
  folders_.push_back(id);
}

void FolderTrail::back() {
  if (!folders_.empty()) {
    folders_.pop_back();
  }
}

void FolderTrail::reset() {
  folders_.clear();
}

std::optional<std::int64_t> FolderTrail::current() const {
  return folders_.empty() ? std::nullopt : std::optional(folders_.back());
}

}  // namespace myclean::tray_model
