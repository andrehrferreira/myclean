#include "core/user_tasks/recycle.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/support.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"

namespace myclean::user_tasks {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view restore_note = "restore them from the Recycle Bin";

}  // namespace

ActionInfo RecycleItems::describe() const {
  return {.what = "Send " + std::to_string(items_.size()) + " items to the Recycle Bin",
          .why = "the owner deleted them from a report view",
          .risk = Risk::low,
          .tier = Tier::report,
          .context = ExecutionContext::user};
}

Result<void> RecycleItems::check(const RecycleItem& item) const {
  if (!item.identity) {
    return fail(Errc::refused, "the plan recorded no identity for it");
  }
  Result<fs::path> normalized = safety::normalize_path(item.path);
  if (!normalized) {
    return tl::unexpected(normalized.error());
  }
  if (tools_.locations == nullptr) {
    return fail(Errc::invalid_state, "the deny-list's places are unknown");
  }
  if (const std::optional<std::string_view> rule =
          safety::denied_by(*normalized, *tools_.locations)) {
    return fail(Errc::refused, "deny_list:" + std::string(*rule));
  }
  const Result<os::FileStat> status = tools_.file_system->stat(*normalized);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (status->kind != os::EntryKind::file || status->attributes.reparse_point) {
    return fail(Errc::refused, "not a regular file");
  }
  if (status->attributes.cloud_placeholder()) {
    return fail(Errc::refused, "a cloud file: recycling it would delete it in the cloud too");
  }
  if (status->identity != *item.identity) {
    return fail(Errc::refused, "another file is at this path since the list was made");
  }
  return {};
}

Result<Preview> RecycleItems::preview() {
  if (tools_.recycler == nullptr || tools_.file_system == nullptr) {
    return fail(Errc::invalid_state, "the tray cannot send files to the Recycle Bin");
  }
  skipped_.clear();
  Preview preview;
  for (const RecycleItem& item : items_) {
    if (Result<void> checked = check(item); !checked) {
      skipped_.push_back({.target = safety::to_utf8(item.path), .error = checked.error()});
      continue;
    }
    preview.items.push_back(PlanItem{.id = item.id,
                                     .path = item.path,
                                     .identity = *item.identity,
                                     .size = item.size,
                                     .approval = ItemApproval::approved});
  }
  return preview;
}

Result<Undo> RecycleItems::apply(const Preview& preview) {
  if (tools_.recycler == nullptr || tools_.file_system == nullptr) {
    return fail(Errc::invalid_state, "the tray cannot send files to the Recycle Bin");
  }
  std::vector<fs::path> files;
  std::vector<PlanItemId> ids;
  for (const PlanItem& previewed : preview.items) {
    const auto item = std::ranges::find(items_, previewed.id, &RecycleItem::id);
    if (item == items_.end()) {
      return fail(Errc::invalid_argument, "an item that is not in the plan");
    }
    if (Result<void> checked = check(*item); !checked) {
      skipped_.push_back({.target = safety::to_utf8(item->path), .error = checked.error()});
      continue;
    }
    files.push_back(item->path);
    ids.push_back(item->id);
  }
  if (files.empty()) {
    return fail(Errc::not_found, "no item is left to send to the Recycle Bin");
  }
  Result<std::vector<cleanup::Skipped>> left = tools_.recycler->recycle(files, stop_);
  if (!left) {
    return tl::unexpected(left.error());
  }
  Undo undo{.items = {}, .not_reversible = std::string(restore_note)};
  for (std::size_t index = 0; index < files.size(); ++index) {
    const std::string path = safety::to_utf8(files[index]);
    if (std::ranges::find(*left, path, &cleanup::Skipped::target) == left->end()) {
      undo.items.push_back(UndoItem{.item = ids[index], .data = path});
    }
  }
  skipped_.insert(skipped_.end(), left->begin(), left->end());
  if (undo.items.empty()) {
    return fail(Errc::io_error, left->empty()
                                    ? "the shell recycled nothing"
                                    : left->front().target + ": " + left->front().error.message);
  }
  return undo;
}

Result<Verification> RecycleItems::verify(const Undo& undo) {
  double left = 0.0;
  for (const UndoItem& item : undo.items) {
    if (tools_.file_system->stat(cleanup::from_utf8(item.data)).has_value()) {
      left += 1.0;
    }
  }
  const auto sent = static_cast<double>(undo.items.size());
  return Verification{.metric = "files_left",
                      .before = sent,
                      .after = left,
                      .effect = left < sent ? Effect::better : Effect::unchanged};
}

Result<void> RecycleItems::undo(const Undo& /*undo*/) {
  return fail(Errc::not_reversible, std::string(restore_note));
}

}  // namespace myclean::user_tasks
