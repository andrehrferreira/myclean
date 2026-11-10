#pragma once

#include <nlohmann/json_fwd.hpp>

#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/installers.hpp"
#include "core/space/photos.hpp"
#include "core/space/scanner.hpp"
#include "core/space/top.hpp"

// JSON forms of the space results, for IPC and MCP (`nlohmann::json value = result;`). Paths are
// UTF-8 text, times Unix seconds (UTC), sizes bytes; an error is {"code", "message"}.
namespace myclean::space {

// A summary, not the tree: {"root", "size_on_disk", "size", "files", "folders", "links",
// "placeholders", "cancelled", "unreadable": [{"path", "error"}]}.
void to_json(nlohmann::json& json, const ScanResult& scan);

// {"files": [{"path", "size_on_disk", "size", "modified"}],
//  "folders": [{"path", "size_on_disk", "size", "files"}]}
void to_json(nlohmann::json& json, const TopItems& top);

// {"path", "size", "size_on_disk", "created", "modified", "age_days",
//  "origin": {"zone_id": number|null, "host_url": string|null, "referrer_url": string|null}
//  or null when unknown}
void to_json(nlohmann::json& json, const OldDownload& download);

// {"used": [{"path", "kind": "msi"|"exe", "size", "size_on_disk", "installed_as",
//  "product_code" (msi), "publisher" (exe)}], "errors": [{"path", "error"}]}
void to_json(nlohmann::json& json, const InstallerReport& report);

// {"groups": [{"size", "hash" (hex), "reclaimable", "files": [{"path", "size_on_disk",
//  "modified"}]}], "reclaimable" (all groups), "errors": [{"path", "error"}], "cancelled"}
void to_json(nlohmann::json& json, const DuplicateReport& report);

// {"groups": [{"distance", "reclaimable", "files": [{"path", "size_on_disk", "modified", "width",
//  "height", "hash" (hex), "keep": bool}]}], "reclaimable" (all groups), "analysed",
//  "not_analysed": [{"path", "error"}], "cancelled"}. The first file of a group is the one to keep.
void to_json(nlohmann::json& json, const PhotoReport& report);

}  // namespace myclean::space
