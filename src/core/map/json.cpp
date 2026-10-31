#include "core/map/json.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/model/result.hpp"

namespace myclean::map {

namespace {

using Json = nlohmann::json;

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

Json area_of(const DiskMap& map, std::optional<std::size_t> match) {
  return match ? Json(map.areas[map.matches[*match].area].id) : Json(nullptr);
}

// NOLINTNEXTLINE(misc-no-recursion): the depth is bounded by the map's depth option
Json node_json(const DiskMap& map, const Node& node) {
  Json children = Json::array();
  for (const Node& child : node.children) {
    children.push_back(node_json(map, child));
  }
  return Json{{"path", utf8(node.path)},
              {"file", node.file},
              {"size", node.size.count()},
              {"apparent", node.apparent.count()},
              {"unexplained", node.unexplained.count()},
              {"area", area_of(map, node.match)},
              {"matched_here", node.matched_here},
              {"children", std::move(children)}};
}

}  // namespace

void to_json(Json& json, const DiskMap& map) {
  Json categories = Json::array();
  for (const CategoryTotal& total : map.categories) {
    categories.push_back(Json{{"category", to_string(total.category)},
                              {"size", total.size.count()},
                              {"reclaimable", total.reclaimable.count()}});
  }
  Json areas = Json::array();
  for (const AreaTotal& total : map.area_totals) {
    const Area& area = map.areas[total.area];
    areas.push_back(Json{{"id", area.id},
                         {"category", to_string(area.category)},
                         {"label", area.label},
                         {"reclaim", Json{{"kind", to_string(area.reclaim.kind)},
                                          {"ref", area.reclaim.ref},
                                          {"note", area.reclaim.note}}},
                         {"size", total.size.count()},
                         {"matches", total.matches},
                         {"unreadable", total.unreadable}});
  }
  Json matches = Json::array();
  for (const Match& match : map.matches) {
    matches.push_back(Json{{"path", utf8(match.path)},
                           {"file", match.file},
                           {"area", map.areas[match.area].id},
                           {"size", match.size.count()},
                           {"apparent", match.apparent.count()},
                           {"unreadable", match.unreadable}});
  }
  Json unexplained = Json::array();
  for (const Node& node : map.unexplained_folders) {
    unexplained.push_back(Json{{"path", utf8(node.path)},
                               {"size", node.size.count()},
                               {"unexplained", node.unexplained.count()}});
  }
  Json unreadable = Json::array();
  for (const Unreadable& folder : map.unreadable) {
    unreadable.push_back(Json{{"path", utf8(folder.path)},
                              {"error", to_string(folder.error.code)},
                              {"message", folder.error.message},
                              {"area", area_of(map, folder.match)}});
  }
  Json volume = nullptr;
  if (map.volume) {
    const std::uint64_t total = map.volume->total.count();
    const std::uint64_t free = map.volume->free.count();
    volume = Json{{"total", total}, {"free", free}, {"used", total > free ? total - free : 0}};
  }
  json = Json{{"root", utf8(map.root)},
              {"volume_root", map.volume_root},
              {"cancelled", map.cancelled},
              {"min_size", map.options.min_size.count()},
              {"depth", map.options.depth},
              {"volume", std::move(volume)},
              {"scanned", map.scanned.count()},
              {"apparent", map.apparent.count()},
              {"explained", map.explained.count()},
              {"unexplained", map.unexplained.count()},
              {"reclaimable", map.reclaimable.count()},
              {"review", map.review.count()},
              {"not_visible", map.not_visible ? Json(*map.not_visible) : Json(nullptr)},
              {"categories", std::move(categories)},
              {"areas", std::move(areas)},
              {"matches", std::move(matches)},
              {"unexplained_folders", std::move(unexplained)},
              {"unreadable", std::move(unreadable)},
              {"tree", node_json(map, map.tree)}};
}

}  // namespace myclean::map
