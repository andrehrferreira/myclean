#include "core/safety/safe_remove.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/plan_source.hpp"

namespace myclean::safety {

namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 6> disposition_names = {
    "removed", "moved", "partial", "skipped", "refused", "failed",
};

std::string deny_reason(std::string_view rule) {
  return "deny_list:" + std::string(rule);
}

// The progress of a caller that wants none.
class Quiet final : public IProgress {
 public:
  Quiet() = default;
  void begin(std::size_t /*items*/, Bytes /*bytes*/) override {}
  void item_started(const PlanItem& /*item*/) override {}
  void file_done(Bytes /*bytes*/) override {}
  void item_finished(const ItemOutcome& /*outcome*/) override {}
};

// Item `id` of `plan` as stored; only the id when the plan has none such.
PlanItem stored_item(const Result<Plan>& plan, PlanItemId id) {
  if (plan) {
    if (const auto found = std::ranges::find(plan->items, id, &PlanItem::id);
        found != plan->items.end()) {
      return *found;
    }
  }
  return PlanItem{.id = id};
}

}  // namespace

std::string_view to_string(Disposition disposition) {
  return detail::enum_name(disposition_names, disposition);
}

std::optional<Disposition> parse_disposition(std::string_view text) {
  return detail::enum_from_name<Disposition>(disposition_names, text);
}

Result<SafeRemove> SafeRemove::create(os::IFileSystem& file_system, const IPlanSource& plans,
                                      ProtectedLocations locations) {
  if (locations.windows.empty() || locations.profiles.empty()) {
    return fail(Errc::invalid_argument, "the Windows and profiles folders are required");
  }
  std::vector<fs::path*> paths = {&locations.windows, &locations.profiles};
  if (!locations.owner_profile.empty()) {
    paths.push_back(&locations.owner_profile);
  }
  for (fs::path& path : locations.owner_known_folders) {
    paths.push_back(&path);
  }
  for (fs::path& path : locations.myclean) {
    paths.push_back(&path);
  }
  for (fs::path* path : paths) {
    Result<fs::path> normal = normalize_path(*path);
    if (!normal) {
      return tl::unexpected(normal.error());
    }
    *path = *std::move(normal);
  }
  return SafeRemove(file_system, plans, std::move(locations));
}

SafeRemove::SafeRemove(os::IFileSystem& file_system, const IPlanSource& plans,
                       ProtectedLocations locations)
    : file_system_(&file_system), plans_(&plans), locations_(std::move(locations)) {}

std::vector<ItemOutcome> SafeRemove::remove(PlanId plan, std::span<const PlanItemId> items,
                                            IProgress* progress) {
  const Run run = begin(plan);
  Quiet quiet;
  IProgress& report = progress != nullptr ? *progress : quiet;
  std::vector<PlanItem> stored;
  stored.reserve(items.size());
  Bytes bytes;
  for (const PlanItemId item : items) {
    bytes += stored.emplace_back(stored_item(run.plan, item)).size;
  }
  report.begin(items.size(), bytes);
  std::vector<ItemOutcome> outcomes;
  outcomes.reserve(items.size());
  for (std::size_t index = 0; index < items.size(); ++index) {
    report.item_started(stored[index]);
    outcomes.push_back(remove_item(run, items[index], report));
    report.item_finished(outcomes.back());
  }
  return outcomes;
}

std::vector<ItemOutcome> SafeRemove::move(PlanId plan, std::span<const MoveRequest> requests,
                                          IProgress* progress) {
  const Run run = begin(plan);
  Quiet quiet;
  IProgress& report = progress != nullptr ? *progress : quiet;
  std::vector<PlanItem> stored;
  stored.reserve(requests.size());
  Bytes bytes;
  for (const MoveRequest& request : requests) {
    bytes += stored.emplace_back(stored_item(run.plan, request.item)).size;
  }
  report.begin(requests.size(), bytes);
  std::vector<ItemOutcome> outcomes;
  outcomes.reserve(requests.size());
  for (std::size_t index = 0; index < requests.size(); ++index) {
    report.item_started(stored[index]);
    outcomes.push_back(move_item(run, requests[index], report));
    report.item_finished(outcomes.back());
  }
  return outcomes;
}

SafeRemove::Verdict SafeRemove::failure(const Error& error) {
  return {.disposition = Disposition::failed,
          .reason = std::string(to_string(error.code)),
          .detail = error.message};
}

SafeRemove::Verdict SafeRemove::source_failure(const Error& error) {
  if (error.code == Errc::not_found) {
    return {.disposition = Disposition::skipped, .reason = "not_found", .detail = error.message};
  }
  return failure(error);
}

SafeRemove::Run SafeRemove::begin(PlanId plan) const {
  // The protected folders are spelled as the file system stores them, so that a comparison
  // with an item's long form cannot miss on case or 8.3 names. Missing folders stay as given.
  Run run{.plan = plans_->load(plan), .locations = locations_};
  const auto spell = [this](fs::path& location) {
    if (location.empty()) {
      return;
    }
    if (Result<fs::path> spelled = long_form(location)) {
      location = *std::move(spelled);
    }
  };
  spell(run.locations.windows);
  spell(run.locations.profiles);
  spell(run.locations.owner_profile);
  std::ranges::for_each(run.locations.owner_known_folders, spell);
  std::ranges::for_each(run.locations.myclean, spell);
  return run;
}

Result<fs::path> SafeRemove::long_form(const fs::path& path) const {
  Result<fs::path> expanded = file_system_->expand_short_names(path);
  if (!expanded) {
    return expanded;
  }
  return normalize_path(*expanded);
}

tl::expected<const PlanItem*, SafeRemove::Verdict> SafeRemove::find_item(const Run& run,
                                                                         PlanItemId id) {
  if (!run.plan) {
    if (run.plan.error().code == Errc::not_found) {
      return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                    .reason = "not_in_plan",
                                    .detail = run.plan.error().message});
    }
    return tl::unexpected(failure(run.plan.error()));
  }
  const Plan& plan = *run.plan;
  const auto found = std::ranges::find(plan.items, id, &PlanItem::id);
  if (found == plan.items.end()) {
    return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                  .reason = "not_in_plan",
                                  .detail = "plan " + std::to_string(plan.id.value()) +
                                            " has no item " + std::to_string(id.value())});
  }
  if (plan.state != PlanState::applying) {
    return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                  .reason = "plan_not_applying",
                                  .detail = "plan is " + std::string(to_string(plan.state))});
  }
  if (found->approval == ItemApproval::not_approved) {
    return tl::unexpected(
        Verdict{.disposition = Disposition::refused, .reason = "not_approved", .detail = {}});
  }
  return &*found;
}

tl::expected<SafeRemove::Target, SafeRemove::Verdict> SafeRemove::check_source(
    const Run& run, const PlanItem& item) const {
  const auto refused = [](std::string reason, std::string detail) {
    return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                  .reason = std::move(reason),
                                  .detail = std::move(detail)});
  };

  // The spelling first, so that a denied path is refused without touching the file system.
  const Result<fs::path> lexical = normalize_path(item.path);
  if (!lexical) {
    return refused("invalid_path", lexical.error().message);
  }
  if (const auto rule = denied_by(*lexical, run.locations)) {
    return refused(deny_reason(*rule), to_utf8(*lexical));
  }
  // Then the long form, which no 8.3 alias can disguise.
  const Result<fs::path> path = long_form(*lexical);
  if (!path) {
    return tl::unexpected(source_failure(path.error()));
  }
  if (const auto rule = denied_by(*path, run.locations)) {
    return refused(deny_reason(*rule), to_utf8(*path));
  }
  // A link above the item would make the path lead somewhere else than it says.
  for (fs::path folder = path->parent_path(); folder.has_relative_path();
       folder = folder.parent_path()) {
    const Result<os::FileStat> status = file_system_->stat(folder);
    if (!status) {
      return tl::unexpected(source_failure(status.error()));
    }
    if (status->attributes.reparse_point) {
      return refused("reparse_point_in_path", to_utf8(folder));
    }
  }
  tl::expected<os::FileStat, Verdict> status = current_stat(*path, item.identity);
  if (!status) {
    return tl::unexpected(std::move(status).error());
  }
  if (status->attributes.cloud_placeholder()) {
    return tl::unexpected(Verdict{.disposition = Disposition::skipped,
                                  .reason = "cloud_placeholder",
                                  .detail = to_utf8(*path)});
  }
  return Target{.path = *path, .stat = *status};
}

tl::expected<fs::path, SafeRemove::Verdict> SafeRemove::check_destination(
    const Run& run, const fs::path& destination) const {
  const auto refused = [](std::string reason, const std::string& detail) {
    return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                  .reason = std::move(reason),
                                  .detail = "destination " + detail});
  };

  const Result<fs::path> lexical = normalize_path(destination);
  if (!lexical) {
    return refused("invalid_path", lexical.error().message);
  }
  if (const auto rule = denied_by(*lexical, run.locations)) {
    return refused(deny_reason(*rule), to_utf8(*lexical));
  }
  // Expand 8.3 names in the part that exists; the rest can only be spelled literally.
  fs::path existing = lexical->parent_path();
  fs::path rest = lexical->filename();
  Result<fs::path> expanded = long_form(existing);
  while (!expanded && expanded.error().code == Errc::not_found && existing.has_relative_path()) {
    rest = existing.filename() / rest;
    existing = existing.parent_path();
    expanded = long_form(existing);
  }
  if (!expanded) {
    return tl::unexpected(failure(expanded.error()));
  }
  fs::path full = *expanded / rest;
  if (const auto rule = denied_by(full, run.locations)) {
    return refused(deny_reason(*rule), to_utf8(full));
  }
  return full;
}

tl::expected<os::FileStat, SafeRemove::Verdict> SafeRemove::current_stat(
    const fs::path& path, const FileIdentity& expected) const {
  Result<os::FileStat> status = file_system_->stat(path);
  if (!status) {
    return tl::unexpected(source_failure(status.error()));
  }
  if (status->identity != expected) {
    return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                  .reason = "changed_since_plan",
                                  .detail = to_utf8(path)});
  }
  return *std::move(status);
}

tl::expected<os::FileStat, SafeRemove::Verdict> SafeRemove::check_entry(
    const Run& run, const os::DirEntry& entry) const {
  if (const auto rule = denied_by(entry.path, run.locations)) {
    return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                  .reason = deny_reason(*rule),
                                  .detail = to_utf8(entry.path)});
  }
  tl::expected<os::FileStat, Verdict> status = current_stat(entry.path, entry.stat.identity);
  if (status && status->attributes.cloud_placeholder()) {
    return tl::unexpected(Verdict{.disposition = Disposition::skipped,
                                  .reason = "cloud_placeholder",
                                  .detail = to_utf8(entry.path)});
  }
  return status;
}

std::optional<SafeRemove::Verdict> SafeRemove::remove_checked(const fs::path& path,
                                                              const FileIdentity& expected) {
  const Result<void> removed = file_system_->remove(path, expected);
  if (removed) {
    return std::nullopt;
  }
  if (removed.error().code == Errc::refused) {
    return Verdict{.disposition = Disposition::refused,
                   .reason = "changed_since_plan",
                   .detail = removed.error().message};
  }
  return source_failure(removed.error());
}

SafeRemove::Tally SafeRemove::remove_contents(const Run& run, const fs::path& folder,
                                              IProgress& progress) {
  // Depth first with an explicit stack, so that no folder depth can exhaust the call stack.
  struct Frame {
    fs::path folder;
    FileIdentity identity;
    std::vector<os::DirEntry> entries;
    std::size_t next = 0;
    bool kept = false;  // something inside stays, so the folder stays too
  };

  Tally tally;
  Result<std::vector<os::DirEntry>> entries = file_system_->enumerate(folder);
  if (!entries) {
    tally.keep(failure(entries.error()));
    return tally;
  }
  std::vector<Frame> stack;
  stack.push_back(Frame{.folder = folder, .identity = {}, .entries = *std::move(entries)});

  while (!stack.empty()) {
    Frame& top = stack.back();
    if (top.next == top.entries.size()) {
      const Frame done = std::move(top);
      stack.pop_back();
      if (stack.empty()) {
        break;  // the item itself: the caller removes it
      }
      if (done.kept || tally.keep(remove_checked(done.folder, done.identity))) {
        stack.back().kept = true;
      } else {
        ++tally.removed;
      }
      continue;
    }

    const os::DirEntry entry = top.entries[top.next++];
    std::optional<Verdict> kept;
    if (auto status = check_entry(run, entry); !status) {
      kept = std::move(status).error();
    } else if (status->kind == os::EntryKind::directory && !status->attributes.reparse_point) {
      Result<std::vector<os::DirEntry>> children = file_system_->enumerate(entry.path);
      if (children) {
        stack.push_back(Frame{
            .folder = entry.path, .identity = status->identity, .entries = *std::move(children)});
        continue;
      }
      kept = failure(children.error());
    } else {
      kept = remove_checked(entry.path, status->identity);  // a file, or a link as the link
      if (!kept) {
        tally.freed += status->size_on_disk;
        ++tally.removed;
        progress.file_done(status->size_on_disk);
      }
    }
    if (tally.keep(std::move(kept))) {
      stack.back().kept = true;
    }
  }
  return tally;
}

ItemOutcome SafeRemove::remove_item(const Run& run, PlanItemId id, IProgress& progress) {
  ItemOutcome outcome{.item = id};
  const auto settle = [&outcome](Verdict verdict) {
    outcome.disposition = verdict.disposition;
    outcome.reason = std::move(verdict.reason);
    outcome.detail = std::move(verdict.detail);
    return outcome;
  };

  auto item = find_item(run, id);
  if (!item) {
    return settle(std::move(item).error());
  }
  outcome.path = (*item)->path;
  auto target = check_source(run, **item);
  if (!target) {
    return settle(std::move(target).error());
  }

  std::optional<Verdict> kept;
  if (target->stat.kind == os::EntryKind::directory && !target->stat.attributes.reparse_point) {
    Tally tally = remove_contents(run, target->path, progress);
    outcome.freed = tally.freed;
    if (!tally.first_kept) {
      // A folder that vanished meanwhile counts as removed.
      tally.keep(remove_checked(target->path, target->stat.identity));
    }
    kept = std::move(tally.first_kept);
    if (kept && tally.removed > 0) {
      kept->disposition = Disposition::partial;
    }
  } else {
    kept = remove_checked(target->path, target->stat.identity);  // a file, or a link as the link
    if (!kept) {
      outcome.freed = target->stat.size_on_disk;
      progress.file_done(target->stat.size_on_disk);
    }
  }
  if (kept) {
    return settle(*std::move(kept));
  }
  outcome.disposition = Disposition::removed;
  return outcome;
}

ItemOutcome SafeRemove::move_item(const Run& run, const MoveRequest& request, IProgress& progress) {
  ItemOutcome outcome{.item = request.item};
  const auto settle = [&outcome](Verdict verdict) {
    outcome.disposition = verdict.disposition;
    outcome.reason = std::move(verdict.reason);
    outcome.detail = std::move(verdict.detail);
    return outcome;
  };

  auto item = find_item(run, request.item);
  if (!item) {
    return settle(std::move(item).error());
  }
  outcome.path = (*item)->path;
  auto target = check_source(run, **item);
  if (!target) {
    return settle(std::move(target).error());
  }
  auto destination = check_destination(run, request.destination);
  if (!destination) {
    return settle(std::move(destination).error());
  }
  if (Result<void> made = file_system_->create_directories(destination->parent_path()); !made) {
    return settle(failure(made.error()));
  }
  // Re-validate right before acting: creating the folders took time.
  if (auto status = current_stat(target->path, target->stat.identity); !status) {
    return settle(std::move(status).error());
  }
  const bool link = target->stat.attributes.reparse_point;
  if (target->stat.kind == os::EntryKind::directory && !link) {
    // A folder is renamed on its own volume and copied to another one.
    const Result<os::FileStat> parent = file_system_->stat(destination->parent_path());
    if (!parent) {
      return settle(failure(parent.error()));
    }
    if (parent->identity.volume_serial != target->stat.identity.volume_serial) {
      if (std::optional<Verdict> kept =
              move_across(run, *target, *destination, request, progress, outcome.freed)) {
        return settle(*std::move(kept));
      }
      outcome.disposition = Disposition::moved;
      return outcome;
    }
  }
  const bool regular_file = target->stat.kind == os::EntryKind::file && !link;
  const Result<void> moved = regular_file ? file_system_->move_file(target->path, *destination)
                                          : file_system_->rename(target->path, *destination);
  if (!moved) {
    return settle(failure(moved.error()));
  }
  outcome.disposition = Disposition::moved;
  if (regular_file) {
    progress.file_done(target->stat.size_on_disk);
  }
  const Result<os::FileStat> arrived = file_system_->stat(*destination);
  if (arrived && arrived->identity.volume_serial != target->stat.identity.volume_serial) {
    outcome.freed = target->stat.size_on_disk;
  }
  return outcome;
}

tl::expected<std::vector<SafeRemove::Entry>, SafeRemove::Verdict> SafeRemove::walk(
    const Run& run, const fs::path& folder) const {
  std::vector<Entry> entries;
  std::vector<fs::path> pending = {fs::path{}};  // folders still to list, relative to `folder`
  while (!pending.empty()) {
    const fs::path relative = std::move(pending.back());
    pending.pop_back();
    Result<std::vector<os::DirEntry>> listed =
        file_system_->enumerate(relative.empty() ? folder : folder / relative);
    if (!listed && !relative.empty() && listed.error().code == Errc::not_found) {
      continue;  // a folder inside, removed by someone else meanwhile
    }
    if (!listed) {
      return tl::unexpected(failure(listed.error()));
    }
    for (const os::DirEntry& child : *listed) {
      tl::expected<os::FileStat, Verdict> status = check_entry(run, child);
      if (!status && status.error().reason == "not_found") {
        continue;  // removed by someone else meanwhile
      }
      if (!status) {
        return tl::unexpected(std::move(status).error());
      }
      if (status->attributes.reparse_point) {
        return tl::unexpected(Verdict{.disposition = Disposition::refused,
                                      .reason = "reparse_point_inside",
                                      .detail = to_utf8(child.path)});
      }
      const fs::path name = relative / child.path.filename();
      if (status->kind == os::EntryKind::directory) {
        pending.push_back(name);
      }
      entries.push_back(Entry{.relative = name, .stat = *status, .copy = std::nullopt});
    }
  }
  return entries;
}

std::optional<SafeRemove::Verdict> SafeRemove::move_across(const Run& run, const Target& source,
                                                           const fs::path& destination,
                                                           const MoveRequest& request,
                                                           IProgress& progress, Bytes& freed) {
  tl::expected<std::vector<Entry>, Verdict> walked = walk(run, source.path);
  if (!walked) {
    return std::move(walked).error();
  }
  std::vector<Entry>& entries = *walked;
  const tl::expected<FileIdentity, Verdict> root = create_destination(destination);
  if (!root) {
    return root.error();
  }

  // The copy, durable before the first source entry goes; then the caller records where the
  // item is, and the source is checked again: copying took time.
  std::optional<Verdict> broken = copy_entries(source.path, destination, entries, progress);
  if (!broken && request.on_copied) {
    if (Result<void> told = request.on_copied(*root); !told) {
      broken = failure(told.error());
    }
  }
  if (!broken) {
    if (auto status = current_stat(source.path, source.stat.identity); !status) {
      broken = std::move(status).error();
    }
  }
  if (broken) {
    if (!discard(destination, *root, entries)) {
      broken->detail += "; part of the copy stays at " + to_utf8(destination);
    }
    return broken;
  }

  std::size_t kept = 0;
  broken = remove_source(source, entries, kept);
  if (!broken) {
    for (const Entry& entry : entries) {
      if (entry.copy && entry.stat.kind == os::EntryKind::file) {
        freed += entry.stat.size_on_disk;
      }
    }
    return std::nullopt;
  }
  // Undo the part removed: copy it back, then drop the destination.
  if (!put_back(source.path, destination, std::span<const Entry>(entries).subspan(kept))) {
    broken->detail += "; the item stays complete at " + to_utf8(destination);
  } else if (!discard(destination, *root, entries)) {
    broken->detail += "; part of the copy stays at " + to_utf8(destination);
  }
  return broken;
}

tl::expected<FileIdentity, SafeRemove::Verdict> SafeRemove::create_destination(
    const fs::path& destination) {
  const Result<os::FileStat> there = file_system_->stat(destination);
  if (there) {
    return tl::unexpected(Verdict{.disposition = Disposition::failed,
                                  .reason = "already_exists",
                                  .detail = "destination " + to_utf8(destination)});
  }
  if (there.error().code != Errc::not_found) {
    return tl::unexpected(failure(there.error()));
  }
  if (Result<void> made = file_system_->create_directories(destination); !made) {
    return tl::unexpected(failure(made.error()));
  }
  const Result<os::FileStat> root = file_system_->stat(destination);
  if (!root) {
    return tl::unexpected(failure(root.error()));
  }
  return root->identity;
}

std::optional<SafeRemove::Verdict> SafeRemove::copy_entries(const fs::path& source,
                                                            const fs::path& destination,
                                                            std::vector<Entry>& entries,
                                                            IProgress& progress) {
  std::vector<fs::path> copies;
  for (Entry& entry : entries) {
    const fs::path to = destination / entry.relative;
    if (entry.stat.kind == os::EntryKind::directory) {
      const Result<void> made = file_system_->create_directories(to);
      const Result<os::FileStat> status =
          made ? file_system_->stat(to) : Result<os::FileStat>(tl::unexpected(made.error()));
      if (!status) {
        return failure(status.error());
      }
      entry.copy = status->identity;
      continue;
    }
    const Result<os::FileStat> copy =
        file_system_->copy_file(source / entry.relative, to, entry.stat.identity);
    if (!copy && copy.error().code == Errc::not_found) {
      continue;  // removed by someone else meanwhile: nothing to move
    }
    if (!copy && copy.error().code == Errc::refused) {
      return Verdict{.disposition = Disposition::refused,
                     .reason = "changed_since_plan",
                     .detail = copy.error().message};
    }
    if (!copy) {
      return failure(copy.error());
    }
    entry.copy = copy->identity;
    copies.push_back(to);
    progress.file_done(entry.stat.size_on_disk);
  }
  // One flush for the whole item, never one per file.
  if (Result<void> flushed = file_system_->flush(copies); !flushed) {
    return failure(flushed.error());
  }
  return std::nullopt;
}

std::optional<SafeRemove::Verdict> SafeRemove::remove_source(const Target& source,
                                                             std::span<const Entry> entries,
                                                             std::size_t& kept) {
  // Deepest first (walk order reversed), through the guarded delete. An entry someone else
  // removed meanwhile is gone either way.
  const auto stays = [](const std::optional<Verdict>& verdict) {
    return verdict && verdict->reason != "not_found";
  };
  for (kept = entries.size(); kept > 0; --kept) {
    const Entry& entry = entries[kept - 1];
    if (!entry.copy) {
      continue;
    }
    std::optional<Verdict> verdict =
        remove_checked(source.path / entry.relative, entry.stat.identity);
    if (stays(verdict)) {
      return verdict;
    }
  }
  const std::optional<Verdict> verdict = remove_checked(source.path, source.stat.identity);
  return stays(verdict) ? verdict : std::nullopt;
}

bool SafeRemove::discard(const fs::path& root, const FileIdentity& identity,
                         std::span<const Entry> entries) {
  bool clean = true;
  for (const Entry& entry : std::views::reverse(entries)) {
    if (entry.copy && !file_system_->remove(root / entry.relative, *entry.copy)) {
      clean = false;
    }
  }
  return file_system_->remove(root, identity).has_value() && clean;
}

bool SafeRemove::put_back(const fs::path& source, const fs::path& copy,
                          std::span<const Entry> entries) {
  std::vector<fs::path> restored;
  for (const Entry& entry : entries) {
    if (!entry.copy) {
      continue;
    }
    const fs::path to = source / entry.relative;
    if (entry.stat.kind == os::EntryKind::directory) {
      if (!file_system_->create_directories(to)) {
        return false;
      }
      continue;
    }
    if (!file_system_->copy_file(copy / entry.relative, to, *entry.copy)) {
      return false;
    }
    restored.push_back(to);
  }
  return file_system_->flush(restored).has_value();
}

}  // namespace myclean::safety
