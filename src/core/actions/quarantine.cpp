#include "core/actions/quarantine.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 3> quarantine_state_names = {"held", "restored", "purged"};

std::string text_of(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

fs::path path_of(const std::string& text) {
  return {std::u8string(text.begin(), text.end())};
}

Result<std::vector<QuarantineRecord>> read_records(store::Database& db, std::string_view where,
                                                   std::optional<PlanId> run) {
  Result<store::Statement> query = db.prepare(
      "SELECT id, run_id, item_id, original_path, quarantine_path, volume_serial, file_id, size, "
      "moved_at, purge_after, state FROM quarantine WHERE " +
      std::string(where) + " ORDER BY id");
  if (!query) {
    return tl::unexpected(query.error());
  }
  if (run) {
    query->bind(1, run->value());
  }
  std::vector<QuarantineRecord> records;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    const std::vector<std::uint8_t> file_id = query->column_blob(6);
    const std::optional<QuarantineState> state = parse_quarantine_state(query->column_text(10));
    FileIdentity identity{.volume_serial = static_cast<std::uint64_t>(query->column_int(5))};
    if (file_id.size() != identity.file_id.size() || !state) {
      return fail(Errc::invalid_data, "quarantine row " + std::to_string(query->column_int(0)) +
                                          " has a malformed file id or state");
    }
    std::ranges::copy(file_id, identity.file_id.begin());
    records.push_back(
        QuarantineRecord{.id = query->column_int(0),
                         .run = PlanId{query->column_int(1)},
                         .item = PlanItemId{query->column_int(2)},
                         .original = path_of(query->column_text(3)),
                         .location = path_of(query->column_text(4)),
                         .identity = identity,
                         .size = Bytes{static_cast<std::uint64_t>(query->column_int(7))},
                         .moved_at = store::from_unix_seconds(query->column_int(8)),
                         .purge_after = store::from_unix_seconds(query->column_int(9)),
                         .state = *state});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return records;
}

// Appends every entry in one transaction and then sets the state of the given records.
Result<void> commit(store::Store& store, std::span<const LogEntry> entries,
                    std::span<const std::int64_t> records, QuarantineState state) {
  return store.write([&](store::Database& db) -> Result<void> {
    for (const LogEntry& entry : entries) {
      if (Result<std::int64_t> row = append_log(db, entry); !row) {
        return tl::unexpected(row.error());
      }
    }
    for (const std::int64_t record : records) {
      if (Result<void> set = set_quarantine_state(db, record, state); !set) {
        return set;
      }
    }
    return {};
  });
}

LogEntry run_entry(WallTime time, PlanId plan, const Actor& actor, std::string_view event,
                   const safety::ItemOutcome& outcome, std::string detail) {
  if (!outcome.detail.empty()) {
    detail += ": " + outcome.detail;
  }
  return LogEntry{.time = time,
                  .plan = plan,
                  .item = outcome.item,
                  .actor = actor.to_string(),
                  .event = std::string(event),
                  .outcome = std::string(to_string(outcome.disposition)),
                  .reason = outcome.reason,
                  .detail = std::move(detail),
                  .metrics = "freed=" + std::to_string(outcome.freed.count())};
}

// Appends every entry and removes the records written ahead of moves that did not happen, in
// one transaction.
Result<void> forget(store::Store& store, std::span<const LogEntry> entries,
                    std::span<const std::int64_t> records) {
  return store.write([&](store::Database& db) -> Result<void> {
    for (const LogEntry& entry : entries) {
      if (Result<std::int64_t> row = append_log(db, entry); !row) {
        return tl::unexpected(row.error());
      }
    }
    for (const std::int64_t record : records) {
      if (Result<void> removed = remove_quarantine(db, record); !removed) {
        return removed;
      }
    }
    return {};
  });
}

// Stores every record and fills in its id, before the items move.
Result<void> write_ahead(store::Store& store,
                         std::vector<std::optional<QuarantineRecord>>& records) {
  return store.write([&](store::Database& db) -> Result<void> {
    for (std::optional<QuarantineRecord>& record : records) {
      if (!record) {
        continue;
      }
      Result<std::int64_t> id = add_quarantine(db, *record);
      if (!id) {
        return tl::unexpected(id.error());
      }
      record->id = *id;
    }
    return {};
  });
}

// A folder copied to another volume: its record (written ahead) points at the complete copy
// before the source goes, so a crash in between leaves a record of where the item is.
void record_copies(store::Store& store, std::vector<safety::MoveRequest>& requests,
                   const std::vector<std::optional<QuarantineRecord>>& records) {
  for (std::size_t index = 0; index < requests.size(); ++index) {
    if (const std::optional<QuarantineRecord>& record = records[index]) {
      requests[index].on_copied = [store = &store, id = record->id](const FileIdentity& copy) {
        return store->write(
            [&](store::Database& db) { return set_quarantine_identity(db, id, copy); });
      };
    }
  }
}

// The record of item `item` of stored plan `plan` moved to `location` at `now`; nullopt when the
// plan has no such item (SafeRemove refuses it).
std::optional<QuarantineRecord> record_of(const Plan& plan, PlanItemId item,
                                          const fs::path& location, WallTime now) {
  const auto stored = std::ranges::find(plan.items, item, &PlanItem::id);
  if (stored == plan.items.end()) {
    return std::nullopt;
  }
  return QuarantineRecord{.run = plan.id,
                          .item = item,
                          .original = stored->path,
                          .location = location,
                          .identity = stored->identity,
                          .size = stored->size,
                          .moved_at = now,
                          .purge_after = now + Quarantine::hold_time};
}

// The records of run `run`, or every held record when `run` is nullopt.
Result<std::vector<QuarantineRecord>> load(store::Store& store, std::optional<PlanId> run) {
  Result<std::vector<QuarantineRecord>> records = fail(Errc::io_error, "not read");
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    records = run ? quarantine_records(db, *run) : held_quarantine(db);
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return records;
}

struct Folder {
  std::vector<std::int64_t> records;
  Bytes size;
};

// The run folders to purge, one per run and volume: of every run whose items all passed their
// expiry and every one `early` accepts, or of run `only` whatever its expiry.
std::map<fs::path, Folder> purge_folders(const std::vector<QuarantineRecord>& held, WallTime now,
                                         std::optional<PlanId> only, const PurgeEarly& early) {
  std::map<std::int64_t, std::vector<const QuarantineRecord*>> runs;
  for (const QuarantineRecord& record : held) {
    if (!only || record.run == *only) {
      runs[record.run.value()].push_back(&record);
    }
  }
  std::map<fs::path, Folder> folders;
  for (const auto& [run, records] : runs) {
    const bool expired = only || std::ranges::none_of(records, [&](const QuarantineRecord* record) {
                           return record->purge_after > now;
                         });
    for (const QuarantineRecord* record : records) {
      // <root>\.myclean-quarantine\<run>\<item>\<name>: the run's folder.
      const fs::path path = record->location.parent_path().parent_path();
      if (!expired && !(early && early(path))) {
        continue;
      }
      Folder& folder = folders[path];
      folder.records.push_back(record->id);
      folder.size += record->size;
    }
  }
  return folders;
}

// True when a purged folder is gone: removed now, or already missing.
bool is_gone(const safety::ItemOutcome& outcome) {
  return outcome.disposition == safety::Disposition::removed ||
         (outcome.disposition == safety::Disposition::skipped && outcome.reason == "not_found");
}

}  // namespace

fs::path quarantine_location(const fs::path& original, PlanId run, PlanItemId item,
                             const fs::path& root) {
  fs::path normal = original.lexically_normal();
  if (!normal.has_filename()) {
    normal = normal.parent_path();
  }
  return (root.empty() ? normal.root_path() : root) / quarantine_folder /
         std::to_string(run.value()) / std::to_string(item.value()) / normal.filename();
}

std::string_view to_string(QuarantineState state) {
  return detail::enum_name(quarantine_state_names, state);
}

std::optional<QuarantineState> parse_quarantine_state(std::string_view text) {
  return detail::enum_from_name<QuarantineState>(quarantine_state_names, text);
}

Result<std::int64_t> add_quarantine(store::Database& db, const QuarantineRecord& record) {
  Result<store::Statement> insert = db.prepare(
      "INSERT INTO quarantine(run_id, item_id, original_path, quarantine_path, volume_serial, "
      "file_id, size, moved_at, purge_after, state) "
      "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  insert->bind(1, record.run.value())
      .bind(2, record.item.value())
      .bind(3, text_of(record.original))
      .bind(4, text_of(record.location))
      .bind(5, record.identity.volume_serial)
      .bind(6, std::span<const std::uint8_t>(record.identity.file_id))
      .bind(7, record.size.count())
      .bind(8, store::to_unix_seconds(record.moved_at))
      .bind(9, store::to_unix_seconds(record.purge_after))
      .bind(10, to_string(record.state));
  if (Result<void> ran = insert->run(); !ran) {
    return tl::unexpected(ran.error());
  }
  return db.last_insert_id();
}

Result<void> set_quarantine_state(store::Database& db, std::int64_t id, QuarantineState state) {
  Result<store::Statement> update = db.prepare("UPDATE quarantine SET state = ?2 WHERE id = ?1");
  if (!update) {
    return tl::unexpected(update.error());
  }
  return update->bind(1, id).bind(2, to_string(state)).run();
}

Result<void> set_quarantine_identity(store::Database& db, std::int64_t id,
                                     const FileIdentity& identity) {
  Result<store::Statement> update =
      db.prepare("UPDATE quarantine SET volume_serial = ?2, file_id = ?3 WHERE id = ?1");
  if (!update) {
    return tl::unexpected(update.error());
  }
  return update->bind(1, id)
      .bind(2, identity.volume_serial)
      .bind(3, std::span<const std::uint8_t>(identity.file_id))
      .run();
}

Result<void> remove_quarantine(store::Database& db, std::int64_t id) {
  Result<store::Statement> remove = db.prepare("DELETE FROM quarantine WHERE id = ?1");
  if (!remove) {
    return tl::unexpected(remove.error());
  }
  return remove->bind(1, id).run();
}

Result<std::vector<QuarantineRecord>> quarantine_records(store::Database& db, PlanId run) {
  return read_records(db, "run_id = ?1", run);
}

Result<std::vector<QuarantineRecord>> held_quarantine(store::Database& db) {
  return read_records(db, "state = 'held'", std::nullopt);
}

Result<Plan> Quarantine::start(Plan plan, const Actor& actor) {
  Result<Plan> stored = engine_->submit(std::move(plan), AutomationLevel::automatic);
  if (!stored) {
    return stored;
  }
  if (Result<void> started = engine_->transition(stored->id, PlanState::applying, actor);
      !started) {
    return tl::unexpected(started.error());
  }
  return stored;
}

Result<void> Quarantine::finish(PlanId plan, std::span<const LogEntry> entries,
                                std::span<const std::int64_t> records, QuarantineState state,
                                std::size_t kept, const Actor& actor) {
  if (Result<void> committed = commit(*environment_.store, entries, records, state); !committed) {
    return committed;
  }
  return engine_->transition(plan, kept == 0 ? PlanState::applied : PlanState::failed, actor);
}

Result<void> Quarantine::restore(PlanId run, std::span<const PlanItemId> items, const Actor& actor,
                                 safety::IProgress* progress) {
  if (actor.kind() == Actor::Kind::mcp) {
    return fail(Errc::refused, actor.to_string() + " may not restore quarantined items");
  }
  Result<std::vector<QuarantineRecord>> all = load(*environment_.store, run);
  if (!all) {
    return tl::unexpected(all.error());
  }
  std::vector<QuarantineRecord> records;
  std::ranges::copy_if(*all, std::back_inserter(records), [&](const QuarantineRecord& record) {
    return record.state == QuarantineState::held &&
           (items.empty() || std::ranges::find(items, record.item) != items.end());
  });
  if (records.empty()) {
    return {};
  }

  Plan plan{
      .action = "RestoreQuarantine", .proposer = actor, .created_at = environment_.clock->now()};
  for (const QuarantineRecord& record : records) {
    plan.items.push_back(
        PlanItem{.path = record.location, .identity = record.identity, .size = record.size});
  }
  const Result<Plan> stored = start(std::move(plan), actor);
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  std::vector<safety::MoveRequest> requests;
  requests.reserve(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    requests.push_back({.item = stored->items[index].id, .destination = records[index].original});
  }
  const std::vector<safety::ItemOutcome> outcomes =
      environment_.safe_remove->move(stored->id, requests, progress);

  std::vector<LogEntry> entries;
  std::vector<std::int64_t> restored;
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    const QuarantineRecord& record = records[index];
    entries.push_back(run_entry(
        environment_.clock->now(), stored->id, actor, "restore", outcomes[index],
        "run " + std::to_string(run.value()) + " item " + std::to_string(record.item.value()) +
            " to " + safety::to_utf8(record.original)));
    if (outcomes[index].disposition == safety::Disposition::moved) {
      restored.push_back(record.id);
    }
  }
  const std::size_t kept = records.size() - restored.size();
  if (Result<void> finished =
          finish(stored->id, entries, restored, QuarantineState::restored, kept, actor);
      !finished) {
    return finished;
  }
  if (kept > 0) {
    return fail(Errc::io_error, std::to_string(kept) + " of " + std::to_string(records.size()) +
                                    " items of run " + std::to_string(run.value()) +
                                    " stayed in quarantine");
  }
  return {};
}

Result<std::size_t> Quarantine::purge(const Actor& actor, std::optional<PlanId> run,
                                      const PurgeEarly& early, safety::IProgress* progress) {
  if (actor.kind() == Actor::Kind::mcp) {
    return fail(Errc::refused, actor.to_string() + " may not purge the quarantine");
  }
  if (run && actor.kind() != Actor::Kind::owner) {
    return fail(Errc::refused, "only the owner may purge a run before its expiry");
  }
  const Result<std::vector<QuarantineRecord>> held = load(*environment_.store, std::nullopt);
  if (!held) {
    return tl::unexpected(held.error());
  }
  const WallTime now = environment_.clock->now();
  const std::map<fs::path, Folder> folders = purge_folders(*held, now, run, early);

  // A folder that is already gone needs no plan.
  std::vector<std::int64_t> purged;
  Plan plan{.action = "PurgeQuarantine", .proposer = actor, .created_at = now};
  std::vector<const Folder*> planned;
  for (const auto& [path, folder] : folders) {
    const Result<os::FileStat> status = environment_.file_system->stat(path);
    if (!status && status.error().code != Errc::not_found) {
      return tl::unexpected(status.error());
    }
    if (!status) {
      purged.insert(purged.end(), folder.records.begin(), folder.records.end());
      continue;
    }
    plan.items.push_back(PlanItem{.path = path, .identity = status->identity, .size = folder.size});
    planned.push_back(&folder);
  }
  if (plan.items.empty()) {
    if (Result<void> committed = commit(*environment_.store, {}, purged, QuarantineState::purged);
        !committed) {
      return tl::unexpected(committed.error());
    }
    return purged.size();
  }

  const Result<Plan> stored = start(std::move(plan), actor);
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  std::vector<PlanItemId> ids;
  ids.reserve(stored->items.size());
  for (const PlanItem& item : stored->items) {
    ids.push_back(item.id);
  }
  const std::vector<safety::ItemOutcome> outcomes =
      environment_.safe_remove->remove(stored->id, ids, progress);
  std::vector<LogEntry> entries;
  std::size_t kept = 0;
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    const safety::ItemOutcome& outcome = outcomes[index];
    entries.push_back(run_entry(environment_.clock->now(), stored->id, actor, "purge", outcome,
                                safety::to_utf8(stored->items[index].path)));
    if (is_gone(outcome)) {
      purged.insert(purged.end(), planned[index]->records.begin(), planned[index]->records.end());
    } else {
      ++kept;
    }
  }
  if (Result<void> finished =
          finish(stored->id, entries, purged, QuarantineState::purged, kept, actor);
      !finished) {
    return tl::unexpected(finished.error());
  }
  return purged.size();
}

QuarantineItems::QuarantineItems(const Environment& environment, Quarantine& quarantine,
                                 PlanId plan, Actor actor, QuarantineRoot root)
    : ItemAction(environment, plan, std::move(actor)),
      quarantine_(&quarantine),
      root_(std::move(root)) {}

ActionInfo QuarantineItems::describe() const {
  return {.what = "Move the items of plan " + std::to_string(plan_id().value()) +
                  " to quarantine for 7 days",
          .why = "the owner approved them; they can be restored until the quarantine is purged",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::service};
}

Result<Undo> QuarantineItems::apply(const Preview& preview) {
  Result<Selection> selection = select(preview);
  if (!selection) {
    return tl::unexpected(selection.error());
  }
  const WallTime now = environment().clock->now();
  std::vector<safety::MoveRequest> requests;
  std::vector<std::optional<QuarantineRecord>> records;
  std::vector<PlanItem> moving;
  for (const PlanItem& item : selection->items) {
    tl::expected<fs::path, std::string> root =
        root_ ? root_(item) : tl::expected<fs::path, std::string>{};
    if (!root) {
      selection->refused.push_back(safety::ItemOutcome{.item = item.id,
                                                       .path = item.path,
                                                       .disposition = safety::Disposition::skipped,
                                                       .reason = std::move(root).error()});
      continue;
    }
    const fs::path location = quarantine_location(item.path, plan_id(), item.id, *root);
    requests.push_back({.item = item.id, .destination = location});
    records.push_back(record_of(selection->plan, item.id, location, now));
    moving.push_back(item);
  }
  if (Result<void> written = write_ahead(*environment().store, records); !written) {
    return tl::unexpected(written.error());
  }
  record_copies(*environment().store, requests, records);

  const std::vector<safety::ItemOutcome> outcomes =
      environment().safe_remove->move(plan_id(), requests, progress());
  std::vector<LogEntry> entries;
  std::vector<std::int64_t> unmoved;
  std::vector<std::pair<std::int64_t, FileIdentity>> moved_away;  // to another volume
  Undo undo;
  for (const safety::ItemOutcome& refused : selection->refused) {
    entries.push_back(outcome_entry("apply", refused, {}));
  }
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    const safety::ItemOutcome& outcome = outcomes[index];
    entries.push_back(outcome_entry("apply", outcome, moving[index].path));
    if (outcome.disposition == safety::Disposition::moved) {
      undo.items.push_back(
          {.item = outcome.item, .data = safety::to_utf8(requests[index].destination)});
      const Result<os::FileStat> arrived =
          environment().file_system->stat(requests[index].destination);
      if (const std::optional<QuarantineRecord>& record = records[index];
          record.has_value() && arrived && arrived->identity != record->identity) {
        moved_away.emplace_back(record->id, arrived->identity);
      }
    } else if (records[index]) {
      unmoved.push_back(records[index]->id);
    }
  }
  if (Result<void> logged = forget(*environment().store, entries, unmoved); !logged) {
    return tl::unexpected(logged.error());
  }
  const Result<void> updated = environment().store->write([&](store::Database& db) -> Result<void> {
    for (const auto& [record, identity] : moved_away) {
      if (Result<void> set = set_quarantine_identity(db, record, identity); !set) {
        return set;
      }
    }
    return {};
  });
  if (!updated) {
    return tl::unexpected(updated.error());
  }
  return undo;
}

Result<void> QuarantineItems::undo(const Undo& undo) {
  std::vector<PlanItemId> items;
  items.reserve(undo.items.size());
  for (const UndoItem& item : undo.items) {
    items.push_back(item.item);
  }
  if (items.empty()) {
    return {};
  }
  return quarantine_->restore(plan_id(), items, actor(), progress());
}

}  // namespace myclean::actions
