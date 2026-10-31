#include "core/tray_model/cleaning.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/replies.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;

Bytes bytes_of(const Json& object, const char* name) {
  return Bytes{object.at(name).get<std::uint64_t>()};
}

WallTime time_of(const Json& object, const char* name) {
  return from_unix_seconds(object.at(name).get<std::int64_t>());
}

std::optional<WallTime> optional_time(const Json& object, const char* name) {
  const auto found = object.find(name);
  if (found == object.end() || found->is_null()) {
    return std::nullopt;
  }
  return from_unix_seconds(found->get<std::int64_t>());
}

std::optional<std::string> optional_text(const Json& object, const char* name) {
  const auto found = object.find(name);
  if (found == object.end() || found->is_null()) {
    return std::nullopt;
  }
  return found->get<std::string>();
}

// A PlanSummary; in a plans.get answer "items" is the item list and "items_total" the count.
PlanRow parse_plan(const Json& plan) {
  const Json& items = plan.at("items");
  return PlanRow{.id = plan.at("id").get<std::int64_t>(),
                 .title = plan.at("title").get<std::string>(),
                 .kind = plan.at("kind").get<std::string>(),
                 .tier = plan.at("tier").get<std::string>(),
                 .scope = plan.at("scope").get<std::string>(),
                 .state = plan.at("state").get<std::string>(),
                 .proposer = plan.at("proposer").get<std::string>(),
                 .created_at = time_of(plan, "created_at"),
                 .items = items.is_array() ? plan.at("items_total").get<std::int64_t>()
                                           : items.get<std::int64_t>(),
                 .bytes = bytes_of(plan, "bytes"),
                 .needs_restart = plan.at("needs_restart").get<bool>()};
}

void add(ByteTotals& totals, const std::string& key, Bytes bytes) {
  const auto found = std::ranges::find(totals, key, &ByteTotals::value_type::first);
  if (found == totals.end()) {
    totals.emplace_back(key, bytes);
  } else {
    found->second += bytes;
  }
}

// The result of `parse`, or the exception it threw as invalid data of `method`.
template <typename T, typename Parse>
Result<T> parsed(std::string_view method, const Parse& parse) {
  try {
    if constexpr (std::is_void_v<T>) {
      parse();
      return {};
    } else {
      return parse();
    }
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string(method) + ": " + error.what());
  }
}

void parse_top(const Json& answer, SpaceReply& reply) {
  for (const Json& file : answer.at("files")) {
    reply.top_files.push_back(TopFileRow{.id = file.at("id").get<std::int64_t>(),
                                         .path = file.at("path").get<std::string>(),
                                         .size_on_disk = bytes_of(file, "size_on_disk"),
                                         .modified = time_of(file, "modified")});
  }
  for (const Json& folder : answer.at("folders")) {
    reply.top_folders.push_back(TopFolderRow{.path = folder.at("path").get<std::string>(),
                                             .size_on_disk = bytes_of(folder, "size_on_disk"),
                                             .files = folder.at("files").get<std::uint64_t>()});
  }
}

void parse_downloads(const Json& answer, SpaceReply& reply) {
  for (const Json& file : answer.at("files")) {
    std::optional<std::string> origin;
    if (const Json& zone = file.at("origin"); !zone.is_null()) {
      origin = optional_text(zone, "host_url");
      if (!origin) {
        origin = optional_text(zone, "referrer_url");
      }
    }
    reply.downloads.push_back(OldDownloadRow{.id = file.at("id").get<std::int64_t>(),
                                             .path = file.at("path").get<std::string>(),
                                             .size_on_disk = bytes_of(file, "size_on_disk"),
                                             .age_days = file.at("age_days").get<std::int64_t>(),
                                             .origin = std::move(origin)});
  }
}

void parse_duplicates(const Json& answer, SpaceReply& reply) {
  for (const Json& group : answer.at("groups")) {
    DuplicateGroupRow row{.size = bytes_of(group, "size"),
                          .reclaimable = bytes_of(group, "reclaimable"),
                          .files = {}};
    for (const Json& file : group.at("files")) {
      row.files.push_back(DuplicateFileRow{.id = file.at("id").get<std::int64_t>(),
                                           .path = file.at("path").get<std::string>(),
                                           .modified = time_of(file, "modified")});
    }
    reply.duplicates.push_back(std::move(row));
  }
}

void parse_installers(const Json& answer, SpaceReply& reply) {
  for (const Json& installer : answer.at("used")) {
    reply.installers.push_back(
        UsedInstallerRow{.id = installer.at("id").get<std::int64_t>(),
                         .path = installer.at("path").get<std::string>(),
                         .kind = installer.at("kind").get<std::string>(),
                         .size_on_disk = bytes_of(installer, "size_on_disk"),
                         .installed_as = installer.at("installed_as").get<std::string>()});
  }
}

// The member that holds the result of `list`.
const char* result_member(SpaceList list) {
  switch (list) {
    case SpaceList::duplicates:
      return "groups";
    case SpaceList::used_installers:
      return "used";
    case SpaceList::top:
    case SpaceList::old_downloads:
      break;
  }
  return "files";
}

}  // namespace

Result<CleaningStatus> parse_cleaning_status(const Json& reply) {
  return parsed<CleaningStatus>(ipc::cleaning_status_method, [&] {
    const std::string state = reply.at("state").get<std::string>();
    if (state != "idle" && state != "scanning") {
      throw std::invalid_argument("unknown state \"" + state + "\"");
    }
    const Json& progress = reply.at("progress");
    return CleaningStatus{.scanning = state == "scanning",
                          .progress = progress.is_number() ? progress.get<double>() : 0.0,
                          .last_completed = optional_time(reply, "last_completed"),
                          .candidates = reply.at("candidates").get<std::int64_t>(),
                          .bytes = bytes_of(reply, "bytes"),
                          .next_auto_run = optional_time(reply, "next_auto_run"),
                          .unavailable = optional_text(reply, "unavailable").value_or("")};
  });
}

Result<std::vector<PlanRow>> parse_plans_list(const Json& reply) {
  return parsed<std::vector<PlanRow>>(ipc::plans_list_method, [&] {
    std::vector<PlanRow> plans;
    for (const Json& plan : reply.at("plans")) {
      plans.push_back(parse_plan(plan));
    }
    return plans;
  });
}

Json plan_id_params(std::int64_t id) {
  return Json{{"id", id}};
}

Result<PlanDetail> parse_plans_get(const Json& reply) {
  return parsed<PlanDetail>(ipc::plans_get_method, [&] {
    const Json& freed = reply.at("freed");
    PlanDetail detail{
        .plan = parse_plan(reply),
        .items = {},
        .commands = reply.at("commands").get<std::vector<std::string>>(),
        .undo = reply.at("undo").get<std::string>(),
        .warnings = reply.at("warnings").get<std::vector<std::string>>(),
        .freed = freed.is_null() ? std::nullopt : std::optional(Bytes{freed.get<std::uint64_t>()}),
        .outcome = optional_text(reply, "detail").value_or("")};
    for (const Json& item : reply.at("items")) {
      const Json& approved = item.at("approved");
      detail.items.push_back(PlanItemRow{
          .id = item.at("id").get<std::int64_t>(),
          .path = item.at("path").get<std::string>(),
          .bytes = bytes_of(item, "bytes"),
          .rule = optional_text(item, "rule"),
          .reason = item.at("reason").get<std::string>(),
          .approved = approved.is_null() ? std::nullopt : std::optional(approved.get<bool>())});
    }
    return detail;
  });
}

Selection select_all() {
  return Selection{.all = true, .items = {}, .except = {}};
}

Result<Selection> select_items(const PlanDetail& detail, const std::set<std::int64_t>& ticked) {
  if (ticked.empty()) {
    return fail(Errc::invalid_argument, "no item is selected");
  }
  if (ticked.size() <= ipc::max_record_elements) {
    return Selection{.all = false, .items = {ticked.begin(), ticked.end()}, .except = {}};
  }
  Selection selection{.all = true, .items = {}, .except = {}};
  for (const PlanItemRow& item : detail.items) {
    if (!ticked.contains(item.id)) {
      selection.except.push_back(item.id);
    }
  }
  if (static_cast<std::int64_t>(detail.items.size()) != detail.plan.items ||
      selection.except.size() > ipc::max_record_elements) {
    return fail(Errc::invalid_argument, "select at most " +
                                            std::to_string(ipc::max_record_elements) +
                                            " items, or leave at most that many unselected");
  }
  return selection;
}

Json plans_approve_params(std::int64_t id, const Selection& selection) {
  Json record{{"all", selection.all}};
  if (!selection.items.empty()) {
    record["items"] = selection.items;
  }
  if (!selection.except.empty()) {
    record["except"] = selection.except;
  }
  return Json{{"id", id}, {"selection", std::move(record)}};
}

Json propose_cleanup_params(const std::vector<std::int64_t>& ids) {
  return Json{{"candidates", {{"ids", ids}}}};
}

Result<std::vector<QuarantineRun>> parse_quarantine_list(const Json& reply) {
  return parsed<std::vector<QuarantineRun>>(ipc::quarantine_list_method, [&] {
    std::vector<QuarantineRun> runs;
    for (const Json& run : reply.at("runs")) {
      QuarantineRun row{.plan = run.at("plan").get<std::int64_t>(),
                        .moved_at = time_of(run, "moved_at"),
                        .purge_after = time_of(run, "purge_after"),
                        .bytes = bytes_of(run, "bytes"),
                        .items = {}};
      for (const Json& item : run.at("items")) {
        row.items.push_back(QuarantineItemRow{.item = item.at("item").get<std::int64_t>(),
                                              .original = item.at("original").get<std::string>(),
                                              .bytes = bytes_of(item, "bytes")});
      }
      runs.push_back(std::move(row));
    }
    return runs;
  });
}

Json quarantine_restore_params(std::int64_t plan, std::optional<std::int64_t> item) {
  Json params{{"plan", plan}};
  if (item) {
    params["item"] = *item;
  }
  return params;
}

std::string_view space_method(SpaceList list) {
  switch (list) {
    case SpaceList::top:
      return ipc::space_top_method;
    case SpaceList::old_downloads:
      return ipc::space_old_downloads_method;
    case SpaceList::duplicates:
      return ipc::space_duplicates_method;
    case SpaceList::used_installers:
      break;
  }
  return ipc::space_used_installers_method;
}

Json space_params(SpaceList list, std::string_view volume, bool refresh) {
  Json params = Json::object();
  if (list == SpaceList::top) {
    params["path"] = std::string(volume.substr(0, volume.find(':')));
  }
  if (refresh) {
    params["refresh"] = true;
  }
  return params;
}

Result<void> parse_space(const Json& answer, SpaceReply& reply) {
  return parsed<void>(space_method(reply.list), [&] {
    reply.state = answer.at("state").get<std::string>();
    reply.scanned_at = optional_time(answer, "scanned_at");
    reply.error = optional_text(answer, "error").value_or("");
    if (!answer.contains(result_member(reply.list))) {
      return;  // no result yet: the first analysis runs or failed
    }
    switch (reply.list) {
      case SpaceList::top:
        parse_top(answer, reply);
        break;
      case SpaceList::old_downloads:
        parse_downloads(answer, reply);
        break;
      case SpaceList::duplicates:
        parse_duplicates(answer, reply);
        break;
      case SpaceList::used_installers:
        parse_installers(answer, reply);
        break;
    }
  });
}

const DuplicateFileRow* keep_hint(const DuplicateGroupRow& group) {
  const auto oldest = std::ranges::min_element(group.files, {}, &DuplicateFileRow::modified);
  return oldest == group.files.end() ? nullptr : &*oldest;
}

bool pending(const PlanRow& plan) {
  return plan.state == awaiting_approval;
}

int pending_count(const std::vector<PlanRow>& plans) {
  return static_cast<int>(std::ranges::count_if(plans, pending));
}

bool undoable(const PlanRow& plan) {
  if (plan.state != "applied" && plan.state != "verified") {
    return false;
  }
  return plan.kind == "relocate" || plan.kind == "system_setting" ||
         (plan.kind == "cleanup" && plan.tier == "ask");
}

ByteTotals pending_bytes_by_tier(const std::vector<PlanRow>& plans) {
  ByteTotals totals;
  for (const PlanRow& plan : plans) {
    if (pending(plan)) {
      add(totals, plan.tier, plan.bytes);
    }
  }
  return totals;
}

ByteTotals pending_bytes_by_kind(const std::vector<PlanRow>& plans) {
  ByteTotals totals;
  for (const PlanRow& plan : plans) {
    if (pending(plan)) {
      add(totals, plan.kind, plan.bytes);
    }
  }
  return totals;
}

void CleanupView::apply(CleanupPoll poll) {
  if (poll.status) {
    status_ = poll.status;
  }
  if (poll.plans) {
    plans_ = std::move(poll.plans);
    std::erase_if(details_, [this](const PlanDetail& detail) {
      return std::ranges::find(*plans_, detail.plan.id, &PlanRow::id) == plans_->end();
    });
  }
  for (PlanDetail& detail : poll.details) {
    const auto found = std::ranges::find(details_, detail.plan.id,
                                         [](const PlanDetail& held) { return held.plan.id; });
    if (found == details_.end()) {
      details_.push_back(std::move(detail));
    } else {
      *found = std::move(detail);
    }
  }
  if (poll.quarantine) {
    quarantine_ = std::move(poll.quarantine);
  }
  if (poll.action) {
    action_error_ = std::move(*poll.action);
  }
  for (SpaceReply& reply : poll.space) {
    space_.at(static_cast<std::size_t>(reply.list)) = std::move(reply);
  }
  if (poll.task_activity) {
    task_activity_ = std::move(*poll.task_activity);
  }
  if (poll.task_note) {
    task_note_ = std::move(*poll.task_note);
  }
}

const PlanDetail* CleanupView::detail(std::int64_t id) const {
  const auto found =
      std::ranges::find(details_, id, [](const PlanDetail& detail) { return detail.plan.id; });
  return found == details_.end() ? nullptr : &*found;
}

}  // namespace myclean::tray_model
