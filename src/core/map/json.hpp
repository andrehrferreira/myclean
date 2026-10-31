#pragma once

#include <nlohmann/json_fwd.hpp>

#include "core/map/disk_map.hpp"

// The JSON form of a disk map, for `myclean map --json` and MCP. Paths are UTF-8 text, sizes
// bytes. Areas are named by id; "areas" describes every area the other lists name.
//   {"root", "volume_root", "cancelled", "min_size", "depth",
//    "volume": {"total", "free", "used"} | null,
//    "scanned", "apparent", "explained", "unexplained", "reclaimable", "review",
//    "not_visible": bytes | null,
//    "categories": [{"category", "size", "reclaimable"}],
//    "areas": [{"id", "category", "label", "reclaim": {"kind", "ref", "note"}, "size",
//               "matches", "unreadable"}],
//    "matches": [{"path", "file", "area", "size", "apparent", "unreadable"}],
//    "unexplained_folders": [{"path", "size", "unexplained"}],
//    "unreadable": [{"path", "error", "message", "area": id | null}],
//    "tree": {"path", "file", "size", "apparent", "unexplained", "area": id | null,
//             "matched_here", "children": [...]}}
namespace myclean::map {

void to_json(nlohmann::json& json, const DiskMap& map);

}  // namespace myclean::map
