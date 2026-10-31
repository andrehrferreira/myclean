#include "core/scanall/sources.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clean/engine.hpp"
#include "core/compress/analyzer.hpp"
#include "core/compress/compression.hpp"
#include "core/drives/analysis.hpp"
#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/model/action.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

// The page file kept on the system drive, so that crash dumps can still be written.
constexpr Bytes kept_page_file{std::uint64_t{4096} << 20U};

std::string join(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += (text.empty() ? "" : "; ") + line;
  }
  return text;
}

Finding match_base(const map::Match& match, const map::Area& area, const DriveInfo& drive) {
  return Finding{
      .group = ActionGroup::manual_review,
      .source = "map",
      .drive = drive.root,
      .path = match.path,
      .items = {Item{.path = match.path, .size = match.size}},
      .bytes = match.size,
      .confidence = 0.5,
      .reason = area.label + (area.reclaim.note.empty() ? "" : ": " + area.reclaim.note)};
}

// nullopt when the rule engine lists the rule's items itself.
std::optional<Finding> clean_rule_finding(Finding finding, const map::Area& area,
                                          std::span<const rules::CleaningRule> rules) {
  const std::string& id = area.reclaim.ref;
  const auto rule = std::ranges::find(rules, id, &rules::CleaningRule::id);
  if (rule == rules.end()) {
    finding.reason += "; its rule " + id + " is not loaded";
    return finding;
  }
  if (rule->strategy == rules::Strategy::native) {
    finding.plan = Plan{.kind = PlanKind::native_command,
                        .params = {{"rule", id}, {"command", rule->command}}};
  } else if (rule->strategy == rules::Strategy::handler) {
    finding.plan = Plan{.kind = PlanKind::cleanup_handler,
                        .params = {{"rule", id}, {"handler", rule->handler}}};
  } else {
    return std::nullopt;
  }
  finding.group = ActionGroup::remove;
  finding.reason += "; it frees what it finds removable, at most this";
  return finding;
}

std::optional<Finding> setting_finding(Finding finding, const map::Area& area,
                                       std::span<const DriveInfo> drives) {
  const std::string& setting = area.reclaim.ref;
  if (setting == "hibernation") {
    finding.group = ActionGroup::remove;
    finding.bytes = Bytes{finding.bytes.count() / 2};
    finding.confidence = 0.7;
    finding.plan = Plan{.kind = PlanKind::hibernation, .params = {{"mode", "reduced"}}};
  } else if (setting == "shadow_storage") {
    finding.group = ActionGroup::remove;
    finding.reason += "; capping it frees at most this";
    finding.plan = Plan{.kind = PlanKind::shadow_storage, .params = {}};
  } else if (setting == "page_file") {
    if (finding.bytes <= kept_page_file) {
      return std::nullopt;
    }
    finding.bytes = Bytes{finding.bytes.count() - kept_page_file.count()};
    const DriveInfo* target = best_target(drives, finding.path, finding.bytes, internal_classes);
    if (target == nullptr) {
      finding.reason += "; no healthy drive keeps 10 % free after taking it";
      return finding;
    }
    finding.group = ActionGroup::relocate;
    finding.plan = Plan{.kind = PlanKind::page_file,
                        .params = {{"keep_mb", "4096"}, {"target", space::utf8(target->root)}}};
  }
  return finding;
}

std::optional<Finding> match_finding(const map::Match& match, const map::Area& area,
                                     const DriveInfo& drive,
                                     std::span<const rules::CleaningRule> rules,
                                     std::span<const DriveInfo> drives) {
  Finding finding = match_base(match, area, drive);
  switch (area.reclaim.kind) {
    case map::ReclaimKind::none:
    case map::ReclaimKind::relocate:
      return std::nullopt;
    case map::ReclaimKind::clean_rule:
      return clean_rule_finding(std::move(finding), area, rules);
    case map::ReclaimKind::handler:
      finding.group = ActionGroup::remove;
      finding.plan =
          Plan{.kind = PlanKind::cleanup_handler, .params = {{"handler", area.reclaim.ref}}};
      return finding;
    case map::ReclaimKind::setting:
      return setting_finding(std::move(finding), area, drives);
    case map::ReclaimKind::uninstall:
      finding.group = ActionGroup::uninstall_review;
      finding.plan = Plan{.kind = PlanKind::uninstall, .params = {{"program", area.label}}};
      return finding;
    case map::ReclaimKind::manual_review:
      return finding;
  }
  return std::nullopt;
}

bool covered(std::span<const Finding> earlier, const clean::Candidate& candidate) {
  return std::ranges::any_of(earlier, [&candidate](const Finding& finding) {
    const std::string* rule = finding.plan.param("rule");
    const std::string* handler = finding.plan.param("handler");
    return (rule != nullptr && *rule == candidate.rule_id) ||
           (handler != nullptr && !candidate.handler.empty() && *handler == candidate.handler);
  });
}

Plan rule_plan(const clean::Candidate& candidate) {
  Plan plan{.kind = PlanKind::delete_items, .params = {{"rule", candidate.rule_id}}};
  switch (candidate.strategy) {
    case rules::Strategy::remove:
      plan.params.emplace_back("path", space::utf8(candidate.path));
      break;
    case rules::Strategy::quarantine:
      plan.kind = PlanKind::quarantine_items;
      plan.params.emplace_back("path", space::utf8(candidate.path));
      break;
    case rules::Strategy::native:
      plan.kind = PlanKind::native_command;
      plan.params.emplace_back("command", candidate.command);
      break;
    case rules::Strategy::handler:
      plan.kind = PlanKind::cleanup_handler;
      plan.params.emplace_back("handler", candidate.handler);
      break;
  }
  if (!candidate.project.empty()) {
    plan.params.emplace_back("project", space::utf8(candidate.project));
  }
  return plan;
}

double tier_confidence(Tier tier) {
  switch (tier) {
    case Tier::automatic:
      return 0.95;
    case Tier::ask:
      return 0.85;
    case Tier::report:
      break;
  }
  return 0.5;
}

std::string rule_reason(const clean::Candidate& candidate) {
  std::string reason = std::format("rule {} ({}, tier {})", candidate.rule_id, candidate.category,
                                   to_string(candidate.tier));
  if (candidate.newest != WallTime{}) {
    reason += std::format("; unchanged since {:%Y-%m-%d}",
                          std::chrono::floor<std::chrono::days>(candidate.newest));
  }
  if (!candidate.project.empty()) {
    reason += "; its project " + space::utf8(candidate.project) + " is inactive";
  }
  return reason;
}

}  // namespace

std::vector<Finding> map_findings(const map::DiskMap& map, const DriveInfo& drive,
                                  std::span<const rules::CleaningRule> rules,
                                  std::span<const DriveInfo> drives, Bytes min_size) {
  std::vector<Finding> findings;
  for (const map::Match& match : map.matches) {
    if (match.size < min_size) {
      continue;
    }
    if (std::optional<Finding> finding =
            match_finding(match, map.areas[match.area], drive, rules, drives)) {
      findings.push_back(*std::move(finding));
    }
  }
  for (const map::Node& node : map.unexplained_folders) {
    if (node.unexplained < min_size) {
      continue;
    }
    findings.push_back(Finding{.group = ActionGroup::manual_review,
                               .source = "map",
                               .drive = drive.root,
                               .path = node.path,
                               .items = {Item{.path = node.path, .size = node.unexplained}},
                               .bytes = node.unexplained,
                               .confidence = 0.3,
                               .reason = "no attribution explains it"});
  }
  return findings;
}

std::vector<Finding> rule_findings(const clean::Evaluation& evaluation, std::string_view source,
                                   std::span<const Finding> earlier,
                                   std::span<const DriveInfo> drives) {
  const auto system = std::ranges::find_if(drives, &DriveInfo::system);
  const DriveInfo* system_drive = system == drives.end() ? nullptr : &*system;
  std::vector<Finding> findings;
  for (const clean::Candidate& candidate : evaluation.candidates) {
    const bool whole_rule = candidate.path.empty();
    const DriveInfo* drive = whole_rule ? system_drive : drive_of(drives, candidate.path);
    if (candidate.size.count() == 0 || drive == nullptr ||
        (whole_rule && covered(earlier, candidate))) {
      continue;
    }
    Finding finding{
        .group = candidate.tier == Tier::report ? ActionGroup::manual_review : ActionGroup::remove,
        .source = std::string(source),
        .drive = drive->root,
        .path = candidate.path,
        .bytes = candidate.size,
        .confidence = tier_confidence(candidate.tier),
        .reason = rule_reason(candidate),
        .plan = rule_plan(candidate)};
    if (!whole_rule) {
      finding.items.push_back(Item{.path = candidate.path, .size = candidate.size});
    }
    findings.push_back(std::move(finding));
  }
  return findings;
}

std::optional<Finding> compress_finding(const compress::FolderEstimate& estimate,
                                        const DriveInfo& drive) {
  if (!estimate.recommended) {
    return std::nullopt;
  }
  const compress::AlgorithmEstimate* chosen = estimate.find(*estimate.recommended);
  if (chosen == nullptr || chosen->savings.count() == 0) {
    return std::nullopt;
  }
  return Finding{
      .group = ActionGroup::compress,
      .source = "compress",
      .drive = drive.root,
      .path = estimate.folder,
      .items = {Item{.path = estimate.folder, .size = estimate.size_on_disk}},
      .bytes = chosen->savings,
      .confidence = chosen->confidence,
      .reason = std::format("{} folder: {} eligible in {} files, {} freed (95 % {} to {}); {}",
                            compress::to_string(estimate.category), size_text(estimate.eligible),
                            estimate.eligible_files, size_text(chosen->savings),
                            size_text(chosen->savings_low), size_text(chosen->savings_high),
                            estimate.reason),
      .plan = Plan{.kind = PlanKind::compress_folder,
                   .params = {{"folder", space::utf8(estimate.folder)},
                              {"algorithm", std::string(compress::to_string(chosen->algorithm))}}}};
}

std::optional<Finding> maintenance_finding(const drives::VolumeAnalysis& analysis) {
  Finding finding{.source = "maintenance",
                  .drive = analysis.drive.root,
                  .confidence = 0.9,
                  .reason = join(analysis.reasons)};
  const std::string volume = space::utf8(analysis.drive.root);
  switch (analysis.recommended) {
    case drives::Maintenance::retrim:
      finding.group = ActionGroup::defrag_trim;
      finding.plan = Plan{.kind = PlanKind::retrim_volume, .params = {{"volume", volume}}};
      return finding;
    case drives::Maintenance::defrag:
      finding.group = ActionGroup::defrag_trim;
      finding.plan = Plan{.kind = PlanKind::defrag_volume, .params = {{"volume", volume}}};
      return finding;
    case drives::Maintenance::none:
      break;
  }
  if (analysis.needed == drives::Maintenance::none) {
    return std::nullopt;
  }
  finding.confidence = 0.5;
  return finding;
}

}  // namespace myclean::scanall
