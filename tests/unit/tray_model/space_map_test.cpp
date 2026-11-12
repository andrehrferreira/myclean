// The Space treemap (SPC-1): one level of a space.top answer as rectangles, and the trail of
// folders the owner opened.

#include "core/tray_model/space_map.hpp"

#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/units.hpp"
#include "core/space/treemap.hpp"
#include "core/tray_model/cleaning.hpp"
#include "inbox_fixtures.hpp"

namespace myclean::tray_model {
namespace {

using Json = nlohmann::json;

const space::Rect bounds{.x = 10, .y = 20, .width = 600, .height = 300};

// C:\ of 100 units: Windows 50 and Users 30 listed, 12 in files directly inside, 8 in children
// the answer leaves out.
SpaceReply volume_reply() {
  Json answer = testing::space_top_json();
  answer["size_on_disk"] = 100;
  answer["own_size_on_disk"] = 12;
  answer["children"] = Json::array({
      Json{{"id", 7}, {"path", R"(C:\Windows)"}, {"size_on_disk", 50}, {"size", 50}, {"files", 3}},
      Json{{"id", 8}, {"path", R"(C:\Users\)"}, {"size_on_disk", 30}, {"size", 30}, {"files", 2}},
      Json{{"id", 9}, {"path", R"(C:\Empty)"}, {"size_on_disk", 0}, {"size", 0}, {"files", 0}},
  });
  SpaceReply reply{.list = SpaceList::top, .volume = "C:"};
  EXPECT_TRUE(parse_space(answer, reply).has_value());
  return reply;
}

TEST(SpaceMapTest, TheAnswerCarriesFolderIdsChildrenAndTotals) {
  const SpaceReply reply = volume_reply();
  EXPECT_EQ(reply.top_path, "C:\\");
  EXPECT_EQ(reply.top_size, Bytes{100});
  EXPECT_EQ(reply.top_own_size, Bytes{12});
  ASSERT_EQ(reply.top_children.size(), 3U);
  EXPECT_EQ(reply.top_children[0].id, 7);
  // An older service gives no folder ids: the folders show, but cannot be opened.
  EXPECT_EQ(reply.top_folders[0].id, 0);

  EXPECT_EQ(space_params(SpaceList::top, "C:", false, 7), (Json{{"folder", 7}}));
  EXPECT_EQ(space_params(SpaceList::top, "C:", true, 7), (Json{{"folder", 7}, {"refresh", true}}));
}

TEST(SpaceMapTest, CellsAreTheChildrenTheOwnFilesAndTheRestEachWithItsShare) {
  const std::vector<TreemapCell> cells = treemap_cells(volume_reply(), bounds);

  ASSERT_EQ(cells.size(), 4U) << "the empty folder gets no cell";
  EXPECT_EQ(cells[0].kind, TreemapCell::Kind::folder);
  EXPECT_EQ(cells[0].folder, 7);
  EXPECT_EQ(cells[0].label, "Windows");
  EXPECT_EQ(cells[1].label, "Users");
  EXPECT_EQ(cells[2].kind, TreemapCell::Kind::files);
  EXPECT_EQ(cells[2].size_on_disk, Bytes{12});
  EXPECT_EQ(cells[3].kind, TreemapCell::Kind::other);
  EXPECT_EQ(cells[3].size_on_disk, Bytes{8});
  double covered = 0.0;
  for (const TreemapCell& cell : cells) {
    EXPECT_NEAR(cell.rect.area(),
                static_cast<double>(cell.size_on_disk.count()) / 100.0 * bounds.area(), 1e-6);
    covered += cell.rect.area();
  }
  EXPECT_NEAR(covered, bounds.area(), 1e-6);

  // A click inside a cell finds it; outside the map, nothing.
  const space::Rect& windows = cells[0].rect;
  EXPECT_EQ(cell_at(cells, windows.x + 1, windows.y + 1), cells.data());
  EXPECT_EQ(cell_at(cells, 0, 0), nullptr);
}

TEST(SpaceMapTest, AFolderWithNothingInItHasNoCells) {
  const SpaceReply empty{.list = SpaceList::top};
  EXPECT_TRUE(treemap_cells(empty, bounds).empty());
}

TEST(SpaceMapTest, TheTrailGoesInsideAndBack) {
  FolderTrail trail;
  EXPECT_TRUE(trail.at_volume());
  EXPECT_FALSE(trail.current());
  trail.open(8);
  trail.open(21);
  EXPECT_EQ(trail.current(), 21);
  trail.back();
  EXPECT_EQ(trail.current(), 8);
  trail.back();
  trail.back();
  EXPECT_TRUE(trail.at_volume());
  trail.open(8);
  trail.reset();
  EXPECT_TRUE(trail.at_volume());
}

}  // namespace
}  // namespace myclean::tray_model
