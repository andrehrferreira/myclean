#include "core/report/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/report/snapshot.hpp"

namespace myclean::report {
namespace {

using Json = nlohmann::json;

std::string month_of(const Json& snapshot) {
  return snapshot.value("month", std::string("?"));
}

// Why `name` cannot be compared, or an empty string when both snapshots collected it.
std::string not_comparable(const Json& before, const Json& after, std::string_view name) {
  for (const Json* snapshot : {&before, &after}) {
    const Json found = category(*snapshot, name);
    if (!collected(found)) {
      return "not collected in " + month_of(*snapshot) + ": " +
             found.value("reason", std::string("unknown reason"));
    }
  }
  return {};
}

std::map<std::string, Json> keyed(const Json& list, std::string_view key) {
  std::map<std::string, Json> result;
  for (const Json& item : list) {
    if (item.contains(key) && item.at(key).is_string()) {
      result.emplace(item.at(key).get<std::string>(), item);
    }
  }
  return result;
}

Json set_diff(const Json& before, const Json& after, std::string_view name) {
  if (const std::string why = not_comparable(before, after, name); !why.empty()) {
    return Json{{"not_compared", why}};
  }
  const auto old_items = keyed(category(before, name).value("items", Json::array()), "key");
  const auto new_items = keyed(category(after, name).value("items", Json::array()), "key");
  Json added = Json::array();
  Json removed = Json::array();
  for (const auto& [key, item] : new_items) {
    if (!old_items.contains(key)) {
      added.push_back(item);
    }
  }
  for (const auto& [key, item] : old_items) {
    if (!new_items.contains(key)) {
      removed.push_back(item);
    }
  }
  return Json{{"added", std::move(added)}, {"removed", std::move(removed)}};
}

std::int64_t integer(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_number() ? found->get<std::int64_t>() : 0;
}

Json folders_diff(const Json& before, const Json& after) {
  if (const std::string why = not_comparable(before, after, "top_folders"); !why.empty()) {
    return Json{{"not_compared", why}};
  }
  const auto old_folders =
      keyed(category(before, "top_folders").value("folders", Json::array()), "path");
  const auto new_folders =
      keyed(category(after, "top_folders").value("folders", Json::array()), "path");
  std::vector<Json> grown;
  Json added = Json::array();
  Json gone = Json::array();
  for (const auto& [path, folder] : new_folders) {
    const auto old = old_folders.find(path);
    const std::int64_t size = integer(folder, "size_on_disk");
    if (old == old_folders.end()) {
      added.push_back(Json{{"path", path}, {"after", size}});
      continue;
    }
    const std::int64_t previous = integer(old->second, "size_on_disk");
    if (size > previous) {
      grown.push_back(
          Json{{"path", path}, {"before", previous}, {"after", size}, {"change", size - previous}});
    }
  }
  for (const auto& [path, folder] : old_folders) {
    if (!new_folders.contains(path)) {
      gone.push_back(Json{{"path", path}, {"before", integer(folder, "size_on_disk")}});
    }
  }
  std::ranges::sort(grown, [](const Json& left, const Json& right) {
    return integer(left, "change") > integer(right, "change");
  });
  return Json{{"grown", grown}, {"new", std::move(added)}, {"gone", std::move(gone)}};
}

Json drives_diff(const Json& before, const Json& after) {
  if (const std::string why = not_comparable(before, after, "disk_health"); !why.empty()) {
    return Json{{"not_compared", why}};
  }
  const auto old_drives =
      keyed(category(before, "disk_health").value("drives", Json::array()), "key");
  const auto new_drives =
      keyed(category(after, "disk_health").value("drives", Json::array()), "key");
  std::map<std::string, Json> keys;
  for (const auto& [key, drive] : old_drives) {
    keys.emplace(key, drive);
  }
  for (const auto& [key, drive] : new_drives) {
    keys.insert_or_assign(key, drive);
  }
  Json result = Json::array();
  for (const auto& [key, drive] : keys) {
    const auto old = old_drives.find(key);
    const auto now = new_drives.find(key);
    Json changes = Json::object();
    if (old != old_drives.end() && now != new_drives.end()) {
      for (const auto& [counter, value] : now->second.items()) {
        if (counter == "key" || counter == "model" || counter == "status" || counter == "reasons") {
          continue;
        }
        const Json previous = old->second.value(counter, Json());
        if (previous != value) {
          changes[counter] = Json{{"before", previous}, {"after", value}};
        }
      }
    }
    result.push_back(Json{
        {"key", key},
        {"model", drive.value("model", std::string())},
        {"status_before", old != old_drives.end() ? old->second.value("status", Json()) : Json()},
        {"status_after", now != new_drives.end() ? now->second.value("status", Json()) : Json()},
        {"changes", std::move(changes)}});
  }
  return result;
}

Json free_space_diff(const Json& before, const Json& after) {
  if (const std::string why = not_comparable(before, after, "free_space"); !why.empty()) {
    return Json{{"not_compared", why}};
  }
  const Json old = category(before, "free_space");
  const Json now = category(after, "free_space");
  const std::int64_t previous = integer(old, "free");
  const std::int64_t current = integer(now, "free");
  return Json{{"volume", now.value("volume", std::string())},
              {"before", previous},
              {"after", current},
              {"change", current - previous}};
}

Json count_of(const Json& snapshot, std::string_view name, std::string_view member) {
  const Json found = category(snapshot, name);
  return collected(found) ? found.value(member, Json()) : Json();
}

Json counts_diff(const Json& before, const Json& after) {
  Json counts = Json::object();
  const auto add = [&](std::string_view label, std::string_view name, std::string_view member) {
    counts[std::string(label)] =
        Json{{"before", count_of(before, name, member)}, {"after", count_of(after, name, member)}};
  };
  add("governor_interventions", "governor", "interventions");
  add("freeze_incidents", "freezes", "incidents");
  add("space_freed_bytes", "space_freed", "bytes");
  return counts;
}

}  // namespace

Json diff(const Json& before, const Json& after) {
  Json sets = Json::object();
  for (const std::string_view name : set_categories) {
    sets[std::string(name)] = set_diff(before, after, name);
  }
  return Json{{"from", month_of(before)},
              {"to", month_of(after)},
              {"sets", std::move(sets)},
              {"top_folders", folders_diff(before, after)},
              {"disk_health", drives_diff(before, after)},
              {"free_space", free_space_diff(before, after)},
              {"counts", counts_diff(before, after)}};
}

}  // namespace myclean::report
