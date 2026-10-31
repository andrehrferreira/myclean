#include "core/actions/item_action.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

namespace myclean::actions {

namespace fs = std::filesystem;

Result<Bytes> measure(const os::IFileSystem& file_system, const fs::path& path) {
  const Result<os::FileStat> status = file_system.stat(path);
  if (!status && status.error().code == Errc::not_found) {
    return Bytes{};
  }
  if (!status) {
    return tl::unexpected(status.error());
  }
  Bytes total = status->size_on_disk;
  std::vector<fs::path> folders;
  if (status->kind == os::EntryKind::directory && !status->attributes.reparse_point) {
    folders.push_back(path);
  }
  while (!folders.empty()) {
    const fs::path folder = std::move(folders.back());
    folders.pop_back();
    Result<std::vector<os::DirEntry>> entries = file_system.enumerate(folder);
    if (!entries) {
      if (entries.error().code == Errc::not_found) {
        continue;
      }
      return tl::unexpected(entries.error());
    }
    for (os::DirEntry& entry : *entries) {
      total += entry.stat.size_on_disk;
      if (entry.stat.kind == os::EntryKind::directory && !entry.stat.attributes.reparse_point) {
        folders.push_back(std::move(entry.path));
      }
    }
  }
  return total;
}

ItemAction::ItemAction(const Environment& environment, PlanId plan, Actor actor)
    : environment_(environment), plan_(plan), actor_(std::move(actor)) {}

Result<Plan> ItemAction::load_plan() const {
  Result<Plan> plan = fail(Errc::not_found, "plan not loaded");
  const Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
    plan = store::get_plan(db, plan_);
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return plan;
}

Result<Preview> ItemAction::preview() {
  Result<Plan> plan = load_plan();
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  Preview preview;
  std::ranges::copy_if(plan->items, std::back_inserter(preview.items), [](const PlanItem& item) {
    return item.approval != ItemApproval::not_approved;
  });
  return preview;
}

Result<Verification> ItemAction::verify(const Undo& undo) {
  const Result<Plan> plan = load_plan();
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  Bytes before;
  Bytes after;
  for (const UndoItem& changed : undo.items) {
    const auto item = std::ranges::find(plan->items, changed.item, &PlanItem::id);
    if (item == plan->items.end()) {
      continue;
    }
    const Result<Bytes> now = measure(*environment_.file_system, item->path);
    if (!now) {
      return tl::unexpected(now.error());
    }
    before += item->size;
    after += *now;
  }
  Effect effect = Effect::unchanged;
  if (after < before) {
    effect = Effect::better;
  } else if (after > before) {
    effect = Effect::worse;
  }
  return Verification{.metric = "bytes_at_original_paths",
                      .before = static_cast<double>(before.count()),
                      .after = static_cast<double>(after.count()),
                      .effect = effect};
}

Result<ItemAction::Selection> ItemAction::select(const Preview& preview) const {
  Result<Plan> plan = load_plan();
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  Selection selection{.plan = *std::move(plan), .items = {}, .refused = {}};
  for (const PlanItem& item : preview.items) {
    const auto stored = std::ranges::find(selection.plan.items, item.id, &PlanItem::id);
    if (stored != selection.plan.items.end() &&
        (stored->path != item.path || stored->identity != item.identity)) {
      selection.refused.push_back(
          safety::ItemOutcome{.item = item.id,
                              .path = item.path,
                              .disposition = safety::Disposition::refused,
                              .reason = "preview_mismatch",
                              .detail = "the plan stores " + safety::to_utf8(stored->path)});
      continue;
    }
    selection.items.push_back(item);
  }
  return selection;
}

LogEntry ItemAction::outcome_entry(std::string_view event, const safety::ItemOutcome& outcome,
                                   const fs::path& requested) const {
  return LogEntry{.time = environment_.clock->now(),
                  .plan = plan_,
                  .item = outcome.item,
                  .actor = actor_.to_string(),
                  .event = std::string(event),
                  .outcome = std::string(to_string(outcome.disposition)),
                  .reason = outcome.reason,
                  .detail = safety::to_utf8(outcome.path.empty() ? requested : outcome.path) +
                            (outcome.detail.empty() ? std::string{} : ": " + outcome.detail),
                  .metrics = "freed=" + std::to_string(outcome.freed.count())};
}

}  // namespace myclean::actions
