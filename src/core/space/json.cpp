#include "core/space/json.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/text.hpp"

namespace myclean::space {

namespace {

std::int64_t unix_seconds(WallTime time) {
  return std::chrono::floor<std::chrono::seconds>(time.time_since_epoch()).count();
}

nlohmann::json error_json(const Error& error) {
  return {{"code", to_string(error.code)}, {"message", error.message}};
}

nlohmann::json text_or_null(const std::string& text) {
  return text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
}

nlohmann::json errors_json(const std::vector<ItemError>& errors) {
  nlohmann::json list = nlohmann::json::array();
  for (const ItemError& item : errors) {
    list.push_back({{"path", utf8(item.path)}, {"error", error_json(item.error)}});
  }
  return list;
}

}  // namespace

void to_json(nlohmann::json& json, const ScanResult& scan) {
  const Folder& root = scan.root();
  nlohmann::json unreadable = nlohmann::json::array();
  for (const Folder& folder : scan.folders) {
    if (folder.error) {
      unreadable.push_back({{"path", utf8(folder.path)}, {"error", error_json(*folder.error)}});
    }
  }
  json = {{"root", utf8(root.path)},
          {"size_on_disk", root.size_on_disk.count()},
          {"size", root.size.count()},
          {"files", root.file_count},
          {"folders", root.folder_count},
          {"links", scan.link_count},
          {"placeholders", scan.placeholder_count},
          {"cancelled", scan.cancelled},
          {"unreadable", std::move(unreadable)}};
}

void to_json(nlohmann::json& json, const TopItems& top) {
  nlohmann::json files = nlohmann::json::array();
  for (const TopFile& file : top.files) {
    files.push_back({{"path", utf8(file.path)},
                     {"size_on_disk", file.size_on_disk.count()},
                     {"size", file.size.count()},
                     {"modified", unix_seconds(file.modified)}});
  }
  nlohmann::json folders = nlohmann::json::array();
  for (const TopFolder& folder : top.folders) {
    folders.push_back({{"path", utf8(folder.path)},
                       {"size_on_disk", folder.size_on_disk.count()},
                       {"size", folder.size.count()},
                       {"files", folder.file_count}});
  }
  json = {{"files", std::move(files)}, {"folders", std::move(folders)}};
}

void to_json(nlohmann::json& json, const OldDownload& download) {
  nlohmann::json origin = nullptr;
  if (download.origin) {
    origin = {{"zone_id", download.origin->zone_id ? nlohmann::json(*download.origin->zone_id)
                                                   : nlohmann::json(nullptr)},
              {"host_url", text_or_null(download.origin->host_url)},
              {"referrer_url", text_or_null(download.origin->referrer_url)}};
  }
  json = {{"path", utf8(download.path)},
          {"size", download.size.count()},
          {"size_on_disk", download.size_on_disk.count()},
          {"created", unix_seconds(download.created)},
          {"modified", unix_seconds(download.modified)},
          {"age_days", download.age_days},
          {"origin", std::move(origin)}};
}

void to_json(nlohmann::json& json, const InstallerReport& report) {
  nlohmann::json used = nlohmann::json::array();
  for (const UsedInstaller& installer : report.used) {
    nlohmann::json item = {{"path", utf8(installer.path)},
                           {"kind", to_string(installer.kind)},
                           {"size", installer.size.count()},
                           {"size_on_disk", installer.size_on_disk.count()},
                           {"installed_as", installer.installed_as}};
    if (installer.kind == InstallerKind::msi) {
      item["product_code"] = installer.product_code;
    } else {
      item["publisher"] = installer.publisher;
    }
    used.push_back(std::move(item));
  }
  json = {{"used", std::move(used)}, {"errors", errors_json(report.errors)}};
}

void to_json(nlohmann::json& json, const DuplicateReport& report) {
  nlohmann::json groups = nlohmann::json::array();
  std::uint64_t reclaimable = 0;
  for (const DuplicateGroup& group : report.groups) {
    nlohmann::json files = nlohmann::json::array();
    for (const DuplicateFile& file : group.files) {
      files.push_back({{"path", utf8(file.path)},
                       {"size_on_disk", file.size_on_disk.count()},
                       {"modified", unix_seconds(file.modified)}});
    }
    groups.push_back({{"size", group.size.count()},
                      {"hash", to_hex(group.digest)},
                      {"reclaimable", group.reclaimable.count()},
                      {"files", std::move(files)}});
    reclaimable += group.reclaimable.count();
  }
  json = {{"groups", std::move(groups)},
          {"reclaimable", reclaimable},
          {"errors", errors_json(report.errors)},
          {"cancelled", report.cancelled}};
}

}  // namespace myclean::space
