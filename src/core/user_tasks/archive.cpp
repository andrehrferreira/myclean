#include "core/user_tasks/archive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/plan_source.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/owner_decisions.hpp"

namespace myclean::user_tasks {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t file_id_digits = 32;

std::string utf8(const fs::path& path) {
  return safety::to_utf8(path);
}

fs::path from_utf8(std::string_view text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

std::string totals_text(const placement::TreeTotals& totals) {
  return std::format("{} files, {} bytes", totals.files, totals.bytes.count());
}

// The one plan SafeRemove may act on: the archive's own item, as the tray applies it now.
class OnePlan final : public safety::IPlanSource {
 public:
  OnePlan(PlanId id, PlanItem item)
      : plan_{.id = id,
              .action = "ArchiveFolder",
              .proposer = Actor::owner(),
              .state = PlanState::applying,
              .items = {}} {
    item.approval = ItemApproval::approved;
    plan_.items.push_back(std::move(item));
  }

  [[nodiscard]] Result<Plan> load(PlanId id) const override {
    if (id != plan_.id) {
      return fail(Errc::not_found, "no plan " + std::to_string(id.value()));
    }
    return plan_;
  }

 private:
  Plan plan_;
};

// The undo data of an applied archive.
struct Record {
  fs::path source;
  fs::path target;
  placement::TreeTotals totals;  // what was copied
  Bytes size;                    // the folder's size on disk before
  FileIdentity copy;             // of the archive copy
};

std::string record_json(const Record& record) {
  return Json{{"source", utf8(record.source)},       {"target", utf8(record.target)},
              {"files", record.totals.files},        {"bytes", record.totals.bytes.count()},
              {"size", record.size.count()},         {"volume_serial", record.copy.volume_serial},
              {"file_id", file_id_text(record.copy)}}
      .dump();
}

Result<Record> parse_record(const Undo& undo) {
  if (undo.items.size() != 1) {
    return fail(Errc::invalid_data, "archive undo data must hold exactly one item");
  }
  try {
    const Json json = Json::parse(undo.items.front().data);
    const std::optional<FileIdentity> copy = parse_identity(
        json.at("volume_serial").get<std::uint64_t>(), json.at("file_id").get<std::string>());
    if (!copy) {
      return fail(Errc::invalid_data, "archive undo data has no identity of the copy");
    }
    return Record{.source = from_utf8(json.at("source").get<std::string>()),
                  .target = from_utf8(json.at("target").get<std::string>()),
                  .totals = {.files = json.at("files").get<std::uint64_t>(),
                             .bytes = Bytes{json.at("bytes").get<std::uint64_t>()}},
                  .size = Bytes{json.at("size").get<std::uint64_t>()},
                  .copy = *copy};
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("archive undo data is malformed: ") + error.what());
  }
}

// Copies folder `source` into the empty or missing folder `target` and checks the copy holds
// the same files and bytes; what was copied.
Result<placement::TreeTotals> copy_checked(const placement::Tools& tools, const fs::path& source,
                                           const fs::path& target) {
  const os::IFileSystem& file_system = *tools.file_system;
  if (file_system.stat(target)) {
    Result<std::vector<os::DirEntry>> entries = file_system.enumerate(target);
    if (!entries || !entries->empty()) {
      return fail(Errc::not_empty, utf8(target) + " is not an empty folder");
    }
  }
  Result<placement::TreeTotals> expected = placement::tree_totals(file_system, source);
  if (!expected) {
    return tl::unexpected(expected.error());
  }
  Result<placement::TreeTotals> copied = tools.copier->copy_tree(source, target);
  if (!copied) {
    return fail(copied.error().code, "copying " + utf8(source) + " to " + utf8(target) +
                                         " failed: " + copied.error().message);
  }
  Result<placement::TreeTotals> present = placement::tree_totals(file_system, target);
  if (!present || *present != *expected || *copied != *expected) {
    return fail(Errc::io_error, "the copy at " + utf8(target) + " does not match " + utf8(source) +
                                    " (" + totals_text(*expected) + " expected)");
  }
  return *expected;
}

// SafeRemove's outcome of removing `item` (a folder) of plan `plan`.
Result<safety::ItemOutcome> remove_folder(const ArchiveTools& tools, PlanId plan,
                                          const PlanItem& item) {
  const OnePlan plans(plan, item);
  Result<safety::SafeRemove> remover =
      safety::SafeRemove::create(*tools.placement->file_system, plans, *tools.locations);
  if (!remover) {
    return tl::unexpected(remover.error());
  }
  const std::array<PlanItemId, 1> ids = {item.id};
  std::vector<safety::ItemOutcome> outcomes = remover->remove(plan, ids);
  if (outcomes.empty()) {
    return fail(Errc::io_error, "SafeRemove reported nothing for " + utf8(item.path));
  }
  return outcomes.front();
}

std::string outcome_text(const safety::ItemOutcome& outcome) {
  std::string text = std::string(safety::to_string(outcome.disposition)) + " (" + outcome.reason;
  if (!outcome.detail.empty()) {
    text += ": " + outcome.detail;
  }
  return text + ")";
}

}  // namespace

std::optional<FileIdentity> parse_identity(std::uint64_t volume_serial, std::string_view file_id) {
  if (file_id.size() != file_id_digits) {
    return std::nullopt;
  }
  FileIdentity identity{.volume_serial = volume_serial};
  for (std::size_t index = 0; index < identity.file_id.size(); ++index) {
    unsigned value = 0;
    for (const char digit : file_id.substr(index * 2, 2)) {
      value <<= 4U;
      if (digit >= '0' && digit <= '9') {
        value |= static_cast<unsigned>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        value |= static_cast<unsigned>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        value |= static_cast<unsigned>(digit - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    identity.file_id.at(index) = static_cast<std::uint8_t>(value);
  }
  return identity;
}

std::string file_id_text(const FileIdentity& identity) {
  std::string text;
  for (const std::uint8_t byte : identity.file_id) {
    text += std::format("{:02x}", byte);
  }
  return text;
}

ActionInfo ArchiveAction::describe() const {
  return {.what = "Move " + utf8(archive_.source) + " to the HDD archive " + utf8(archive_.target),
          .why =
              "nothing in it changed for months; the copy is checked before the folder is "
              "removed, and undo copies it back",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::user};
}

Result<Preview> ArchiveAction::preview() {
  Result<fs::path> source = safety::normalize_path(archive_.source);
  if (!source) {
    return tl::unexpected(source.error());
  }
  const os::IFileSystem& file_system = *tools_.placement->file_system;
  Result<os::FileStat> stat = file_system.stat(*source);
  if (!stat) {
    return tl::unexpected(stat.error());
  }
  if (stat->kind != os::EntryKind::directory || stat->attributes.reparse_point) {
    return fail(Errc::invalid_argument, utf8(*source) + " is not a folder");
  }
  Result<Bytes> size = placement::readable_size(file_system, *source);
  if (!size) {
    return tl::unexpected(size.error());
  }
  return Preview{.items = {PlanItem{.path = *source, .identity = stat->identity, .size = *size}}};
}

Result<void> ArchiveAction::check_source(const fs::path& source) const {
  if (scanall::system_location(source)) {
    return fail(Errc::refused, utf8(source) + " holds the system or installed programs");
  }
  if (const std::optional<std::string_view> rule = safety::denied_by(source, *tools_.locations)) {
    return fail(Errc::refused,
                "the deny-list protects " + utf8(source) + " (" + std::string(*rule) + ")");
  }
  for (const fs::path& folder : tools_.usage->process_folders()) {
    if (placement::same_path(folder, source) || placement::inside(folder, source)) {
      return fail(Errc::in_use, "a running program uses " + utf8(folder) + ", inside " +
                                    utf8(source) + "; close it and try again");
    }
  }
  return {};
}

Result<void> ArchiveAction::remove(const PlanItem& item, bool& kept) const {
  Result<safety::ItemOutcome> outcome = remove_folder(tools_, plan_, item);
  if (!outcome) {
    return tl::unexpected(outcome.error());
  }
  switch (outcome->disposition) {
    case safety::Disposition::removed:
      return {};
    case safety::Disposition::partial:
      kept = true;
      return {};
    default:
      break;
  }
  return fail(outcome->disposition == safety::Disposition::refused ? Errc::refused : Errc::io_error,
              "removing " + utf8(item.path) + " was " + outcome_text(*outcome));
}

void ArchiveAction::discard(const fs::path& target, PlanItemId item) const {
  const Result<os::FileStat> stat = tools_.placement->file_system->stat(target);
  if (stat) {
    static_cast<void>(remove_folder(
        tools_, plan_, PlanItem{.id = item, .path = target, .identity = stat->identity}));
  }
}

Result<Undo> ArchiveAction::apply(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "an archive preview holds exactly one item");
  }
  const PlanItem& item = preview.items.front();
  Result<fs::path> source = safety::normalize_path(archive_.source);
  Result<fs::path> target = safety::normalize_path(archive_.target);
  if (!source || !target) {
    return tl::unexpected(source ? target.error() : source.error());
  }
  if (!placement::same_path(item.path, *source)) {
    return fail(Errc::refused, "the preview does not name " + utf8(*source));
  }
  if (expected_ && item.identity != *expected_) {
    return fail(Errc::refused, utf8(*source) + " was replaced since the plan was made");
  }
  if (Result<void> allowed = check_source(*source); !allowed) {
    return tl::unexpected(allowed.error());
  }
  const std::wstring root = placement::drive_root(*target);
  if (root.empty() || root == placement::drive_root(*source) ||
      !placement::same_path(*target, scanall::archive_folder(*source, root))) {
    return fail(Errc::refused, utf8(*target) + " is not the archive folder of " + utf8(*source) +
                                   " on another volume");
  }
  // The target's drive is checked now, not when it was listed: it may have degraded since.
  if (Result<void> safe = placement::check_target(*tools_.placement->drives, *target, item.size);
      !safe) {
    return tl::unexpected(safe.error());
  }
  const os::IFileSystem& file_system = *tools_.placement->file_system;
  Result<placement::TreeTotals> copied = copy_checked(*tools_.placement, *source, *target);
  if (!copied) {
    if (copied.error().code != Errc::not_empty) {  // what is there now is this copy's
      discard(*target, item.id);
    }
    return fail(copied.error().code, copied.error().message + "; nothing was removed");
  }
  const Result<os::FileStat> copy = file_system.stat(*target);
  if (!copy) {
    return tl::unexpected(copy.error());
  }
  // A file written into the folder during the copy would be lost with it.
  if (const Result<placement::TreeTotals> now = placement::tree_totals(file_system, *source);
      !now || *now != *copied) {
    discard(*target, item.id);
    return fail(Errc::refused, utf8(*source) + " changed while it was copied; nothing was removed");
  }
  bool kept = false;
  if (Result<void> removed = remove(item, kept); !removed) {
    const Result<placement::TreeTotals> now = placement::tree_totals(file_system, *source);
    if (now && *now == *copied) {
      discard(*target, item.id);
      return fail(removed.error().code, removed.error().message + "; it stays where it was");
    }
    return fail(removed.error().code, removed.error().message + "; part of it is gone, so the " +
                                          "archive copy at " + utf8(*target) + " is kept");
  }
  const Record record{.source = *source,
                      .target = *target,
                      .totals = *copied,
                      .size = item.size,
                      .copy = copy->identity};
  return Undo{.items = {UndoItem{.item = item.id, .data = record_json(record)}}};
}

Result<Verification> ArchiveAction::verify(const Undo& undo) {
  Result<Record> record = parse_record(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  const os::IFileSystem& file_system = *tools_.placement->file_system;
  if (const Result<placement::TreeTotals> present =
          placement::tree_totals(file_system, record->target);
      !present || *present != record->totals) {
    return fail(Errc::io_error, "the archive copy at " + utf8(record->target) +
                                    " no longer holds " + totals_text(record->totals));
  }
  Result<Bytes> left = placement::readable_size(file_system, record->source);
  if (!left) {
    return tl::unexpected(left.error());
  }
  return Verification{.metric = "source_bytes",
                      .before = static_cast<double>(record->size.count()),
                      .after = static_cast<double>(left->count()),
                      .effect = *left < record->size ? Effect::better : Effect::unchanged};
}

Result<void> ArchiveAction::undo(const Undo& undo) {
  Result<Record> record = parse_record(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  const os::IFileSystem& file_system = *tools_.placement->file_system;
  if (const Result<placement::TreeTotals> present =
          placement::tree_totals(file_system, record->target);
      !present || *present != record->totals) {
    return fail(Errc::invalid_state, "the archive copy at " + utf8(record->target) +
                                         " changed since it was made; nothing was undone");
  }
  bool restored = false;
  if (file_system.stat(record->source)) {
    Result<std::vector<os::DirEntry>> entries = file_system.enumerate(record->source);
    if (!entries) {
      return tl::unexpected(entries.error());
    }
    if (!entries->empty()) {
      const Result<placement::TreeTotals> now = placement::tree_totals(file_system, record->source);
      if (!now || *now != record->totals) {
        return fail(Errc::not_empty,
                    utf8(record->source) + " holds other content now; nothing was undone");
      }
      restored = true;  // a previous undo copied it back already
    }
  }
  const PlanItemId item = undo.items.front().item;
  if (!restored) {
    Result<std::vector<placement::TargetVolume>> volumes = tools_.placement->drives->volumes();
    if (!volumes) {
      return tl::unexpected(volumes.error());
    }
    const placement::TargetVolume* volume = placement::volume_of(*volumes, record->source);
    if (volume == nullptr || volume->free < record->totals.bytes) {
      return fail(Errc::refused, "the volume of " + utf8(record->source) + " lacks the room for " +
                                     totals_text(record->totals) + "; nothing was undone");
    }
    if (Result<placement::TreeTotals> copied =
            copy_checked(*tools_.placement, record->target, record->source);
        !copied) {
      discard(record->source, item);
      return fail(copied.error().code, copied.error().message + "; the archive copy stays");
    }
  }
  bool kept = false;
  if (Result<void> removed =
          remove(PlanItem{.id = item, .path = record->target, .identity = record->copy}, kept);
      !removed) {
    return fail(removed.error().code,
                utf8(record->source) + " is restored, but " + removed.error().message);
  }
  return {};
}

}  // namespace myclean::user_tasks
