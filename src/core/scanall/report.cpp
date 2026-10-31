#include "core/scanall/report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

// `text` with ASCII letters lower-cased and '/' as '\', as paths are compared.
std::wstring fold(std::wstring text) {
  for (wchar_t& c : text) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    } else if (c == L'/') {
      c = L'\\';
    }
  }
  return text;
}

// The comparison key of a path from a finding: normal form, no trailing separator below a root.
std::wstring claim_key(const fs::path& path) {
  std::wstring text = fold(path.lexically_normal().native());
  while (text.size() > 3 && text.back() == L'\\') {
    text.pop_back();
  }
  return text;
}

struct Claim {
  std::size_t finding = 0;
  Bytes size;
  std::wstring key;
  std::size_t depth = 0;  // separators in the key
  bool placed = false;    // found in the tree
};

// Splits each drive's bytes among the findings that cover them (see build_report).
class Ledger {
 public:
  Ledger(const std::vector<Finding>& findings, std::span<const std::size_t> on_drive)
      : findings_(&findings) {
    for (const std::size_t index : on_drive) {
      for (const Item& item : findings[index].items) {
        std::wstring key = claim_key(item.path);
        const auto depth = static_cast<std::size_t>(std::ranges::count(key, L'\\'));
        at_path_[key].push_back(claims_.size());
        claims_.push_back(
            Claim{.finding = index, .size = item.size, .key = std::move(key), .depth = depth});
      }
    }
  }

  // The bytes each finding owns in `tree`, by finding index.
  void credit(const space::ScanResult& tree, std::vector<Bytes>& owned) {
    if (claims_.empty()) {
      return;
    }
    std::vector<std::size_t> owner(tree.folders.size(), none);
    for (std::size_t index = 0; index < tree.folders.size(); ++index) {
      const space::Folder& folder = tree.folders[index];
      owner[index] = index == 0 ? none : owner[folder.parent];
      if (const auto found = at_path_.find(fold(folder.path.native())); found != at_path_.end()) {
        for (const std::size_t claim : found->second) {
          claims_[claim].placed = true;
          owner[index] = winner(claim, owner[index]);
        }
      }
    }
    std::unordered_map<std::wstring, std::vector<std::size_t>> by_parent;
    for (std::size_t claim = 0; claim < claims_.size(); ++claim) {
      if (!claims_[claim].placed) {
        by_parent[claim_key(fs::path{claims_[claim].key}.parent_path())].push_back(claim);
      }
    }
    for (std::size_t index = 0; index < tree.folders.size(); ++index) {
      const space::Folder& folder = tree.folders[index];
      std::uint64_t direct = folder.own_size_on_disk.count();
      if (const auto found = by_parent.find(fold(folder.path.native())); found != by_parent.end()) {
        for (const std::size_t claim : found->second) {
          claims_[claim].placed = true;
          add(owned, winner(claim, owner[index]), claims_[claim].size.count());
          direct -= std::min(direct, claims_[claim].size.count());
        }
      }
      add(owned, owner[index], direct);
    }
    for (std::size_t claim = 0; claim < claims_.size(); ++claim) {
      if (!claims_[claim].placed) {
        add(owned, claim, claims_[claim].size.count());
      }
    }
  }

 private:
  // The claim that owns bytes both `claim` and `holder` (none, or a claim at an ancestor or at
  // the same path) cover.
  [[nodiscard]] std::size_t winner(std::size_t claim, std::size_t holder) const {
    if (holder == none) {
      return claim;
    }
    const auto rank = [this](std::size_t index) {
      const Claim& entry = claims_[index];
      return std::tuple{static_cast<int>((*findings_)[entry.finding].group),
                        -static_cast<std::int64_t>(entry.depth)};
    };
    return rank(claim) < rank(holder) ? claim : holder;
  }

  void add(std::vector<Bytes>& owned, std::size_t claim, std::uint64_t bytes) const {
    if (claim != none) {
      owned[claims_[claim].finding] += Bytes{bytes};
    }
  }

  const std::vector<Finding>* findings_;
  std::vector<Claim> claims_;
  std::unordered_map<std::wstring, std::vector<std::size_t>> at_path_;
};

bool writes_in_place(ActionGroup group) {
  return group == ActionGroup::remove || group == ActionGroup::deduplicate ||
         group == ActionGroup::compress || group == ActionGroup::defrag_trim;
}

void hold_on_unhealthy(std::vector<Finding>& findings, std::span<const DriveInfo> drives) {
  for (Finding& finding : findings) {
    const DriveInfo* drive = drive_of(drives, finding.drive);
    if (drive != nullptr && !healthy(*drive) && writes_in_place(finding.group)) {
      finding.reason =
          std::format("held: the drive's health is {}{}; would be {} by {}: {}",
                      health::to_string(drive->health),
                      drive->health_note.empty() ? "" : " (" + drive->health_note + ")",
                      to_string(finding.group), to_string(finding.plan.kind), finding.reason);
      finding.group = ActionGroup::manual_review;
    }
  }
}

Bytes share(Bytes bytes, Bytes owned, Bytes footprint) {
  if (footprint.count() == 0 || owned >= footprint) {
    return bytes;
  }
  return Bytes{static_cast<std::uint64_t>(
      std::llround(static_cast<double>(bytes.count()) * static_cast<double>(owned.count()) /
                   static_cast<double>(footprint.count())))};
}

std::size_t drive_index(std::span<const DriveSummary> drives, const fs::path& root) {
  const std::wstring wanted = placement::drive_root(root);
  for (std::size_t index = 0; index < drives.size(); ++index) {
    if (placement::drive_root(drives[index].drive.root) == wanted) {
      return index;
    }
  }
  return none;
}

Json by_group(const std::array<Bytes, action_group_count>& bytes) {
  Json json = Json::object();
  for (std::size_t group = 0; group < action_group_count; ++group) {
    json[std::string(to_string(static_cast<ActionGroup>(group)))] = bytes.at(group).count();
  }
  return json;
}

Json drive_json(const DriveSummary& summary) {
  const DriveInfo& drive = summary.drive;
  const std::uint64_t used = drive.size.count() - std::min(drive.size.count(), drive.free.count());
  return Json{{"root", space::utf8(drive.root)},
              {"class", placement::to_string(drive.drive_class)},
              {"health", health::to_string(drive.health)},
              {"health_note", drive.health_note},
              {"system", drive.system},
              {"size", drive.size.count()},
              {"used", used},
              {"free", drive.free.count()},
              {"tree", drive.tree},
              {"complete", drive.complete},
              {"scanned", drive.scanned.count()},
              {"explained", drive.explained.count()},
              {"explained_percent",
               used == 0 ? Json(nullptr)
                         : Json(std::round(static_cast<double>(drive.explained.count()) * 1000.0 /
                                           static_cast<double>(used)) /
                                10.0)},
              {"not_visible", drive.not_visible ? Json(*drive.not_visible) : Json(nullptr)},
              {"notes", drive.notes},
              {"counted", by_group(summary.counted)}};
}

Json finding_json(const Finding& finding) {
  Json params = Json::object();
  for (const auto& [name, value] : finding.plan.params) {
    params[name] = value;
  }
  Json items = Json::array();
  for (const Item& item : finding.items) {
    items.push_back(Json{{"path", space::utf8(item.path)}, {"size", item.size.count()}});
  }
  return Json{{"group", to_string(finding.group)},
              {"source", finding.source},
              {"drive", space::utf8(finding.drive)},
              {"path", space::utf8(finding.path)},
              {"bytes", finding.bytes.count()},
              {"counted", finding.counted.count()},
              {"confidence", finding.confidence},
              {"reason", finding.reason},
              {"urgent", finding.urgent},
              {"plan", Json{{"kind", to_string(finding.plan.kind)}, {"params", params}}},
              {"items", items}};
}

}  // namespace

Report build_report(std::vector<DriveInfo> drives, std::vector<Finding> findings,
                    std::span<const space::ScanResult> trees, WallTime time) {
  hold_on_unhealthy(findings, drives);
  Report report{.time = time};
  for (DriveInfo& drive : drives) {
    report.drives.push_back(DriveSummary{.drive = std::move(drive)});
  }

  std::vector<std::vector<std::size_t>> on_drive(report.drives.size());
  for (std::size_t index = 0; index < findings.size(); ++index) {
    if (const std::size_t drive = drive_index(report.drives, findings[index].drive);
        drive != none) {
      on_drive[drive].push_back(index);
    }
  }
  std::vector<Bytes> owned(findings.size());
  std::vector<bool> accounted(findings.size(), false);
  for (const space::ScanResult& tree : trees) {
    const std::size_t drive = drive_index(report.drives, tree.root().path);
    if (drive == none) {
      continue;
    }
    Ledger(findings, on_drive[drive]).credit(tree, owned);
    for (const std::size_t index : on_drive[drive]) {
      accounted[index] = true;
    }
  }

  for (std::size_t index = 0; index < findings.size(); ++index) {
    Finding& finding = findings[index];
    finding.counted = accounted[index] && !finding.items.empty()
                          ? share(finding.bytes, owned[index], footprint(finding))
                          : finding.bytes;
    const auto group = static_cast<std::size_t>(finding.group);
    report.totals.at(group) += finding.counted;
    if (const std::size_t drive = drive_index(report.drives, finding.drive); drive != none) {
      report.drives[drive].counted.at(group) += finding.counted;
    }
  }
  std::ranges::sort(findings, [](const Finding& left, const Finding& right) {
    return std::tuple{left.group, !left.urgent, right.counted, left.path} <
           std::tuple{right.group, !right.urgent, left.counted, right.path};
  });
  report.findings = std::move(findings);
  return report;
}

Json to_json(const Report& report) {
  Json drives = Json::array();
  for (const DriveSummary& drive : report.drives) {
    drives.push_back(drive_json(drive));
  }
  Json findings = Json::array();
  for (const Finding& finding : report.findings) {
    findings.push_back(finding_json(finding));
  }
  return Json{
      {"time", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(report.time))},
      {"precedence", Json::array({"remove", "deduplicate", "relocate", "archive", "compress",
                                  "defrag-trim", "uninstall-review", "manual-review"})},
      {"drives", drives},
      {"totals", by_group(report.totals)},
      {"findings", findings},
      {"notes", report.notes}};
}

}  // namespace myclean::scanall
