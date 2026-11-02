#include "core/user_tasks/task.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/native_command.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/cleanup/support.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/recipe.hpp"
#include "core/placement/relocation.hpp"
#include "core/safety/paths.hpp"

namespace myclean::user_tasks {

namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 8> user_handlers = {
    "Active Setup Temp Folders", "D3D Shader Cache",    "Downloaded Program Files",
    "Internet Cache Files",      "Offline Pages Files", "Recycle Bin",
    "Temporary Files",           "Thumbnail Cache"};

// The length of the longest prefix of `text` that ends on a UTF-8 character boundary and is at
// most `limit` bytes.
std::size_t cut(std::string_view text, std::size_t limit) {
  if (text.size() <= limit) {
    return text.size();
  }
  std::size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
    --end;  // a continuation byte: cut before its character
  }
  return end;
}

// `text` as a record string: control characters replaced, cut to the record's string size.
std::string bounded(std::string_view text) {
  std::string result;
  for (const char character : text.substr(0, cut(text, ipc::max_record_string))) {
    const auto byte = static_cast<unsigned char>(character);
    result += byte < 0x20 || byte == 0x7F ? ' ' : character;
  }
  return result;
}

template <typename T>
T parse_text(const Json& object, const char* name, std::optional<T> (*parse)(std::string_view)) {
  const std::string text = object.at(name).get<std::string>();
  const std::optional<T> value = parse(text);
  if (!value) {
    throw std::invalid_argument("unknown " + std::string(name) + " \"" + text + "\"");
  }
  return *value;
}

std::optional<TaskKind> parse_kind(std::string_view text) {
  if (text == "native") {
    return TaskKind::native;
  }
  if (text == "handler") {
    return TaskKind::handler;
  }
  if (text == "relocate") {
    return TaskKind::relocate;
  }
  if (text == "recycle_bin") {
    return TaskKind::recycle_bin;
  }
  return std::nullopt;
}

std::optional<Operation> parse_operation(std::string_view text) {
  if (text == "apply") {
    return Operation::apply;
  }
  if (text == "undo") {
    return Operation::undo;
  }
  return std::nullopt;
}

std::filesystem::path path_of(const Json& value) {
  return cleanup::from_utf8(value.get<std::string>());
}

void parse_action(const Json& action, UserTask& task) {
  switch (task.kind) {
    case TaskKind::native:
      task.command = action.at("command").get<std::string>();
      break;
    case TaskKind::handler:
      task.volume = path_of(action.at("volume"));
      task.handlers = action.at("handlers").get<std::vector<std::string>>();
      break;
    case TaskKind::recycle_bin:
      task.deleted_days = action.at("deleted_days").get<int>();
      break;
    case TaskKind::relocate:
      if (const auto recipe = action.find("recipe"); recipe != action.end()) {
        Result<packs::RelocationRecipe> parsed = placement::recipe_from_json(*recipe);
        if (!parsed) {
          throw std::invalid_argument(parsed.error().message);
        }
        task.recipe = placement::RecipeRelocation{
            .recipe = *std::move(parsed),
            .instance = action.at("instance").get<std::string>(),
            .target = path_of(action.at("target")),
            .content = parse_text(action, "content", &placement::parse_content_mode)};
        break;
      }
      task.relocation = placement::Relocation{
          .kind = parse_text(action, "cache", &placement::parse_cache_kind),
          .target = path_of(action.at("target")),
          .content = parse_text(action, "content", &placement::parse_content_mode)};
      break;
  }
}

// The stored undo data; consecutive pieces of one item are joined.
Undo parse_undo(const Json& undo) {
  Undo parsed{.items = {}, .not_reversible = undo.at("not_reversible").get<std::string>()};
  for (const Json& entry : undo.at("items")) {
    const PlanItemId item{entry.at("item").get<std::int64_t>()};
    std::string data = entry.at("data").get<std::string>();
    if (!parsed.items.empty() && parsed.items.back().item == item) {
      parsed.items.back().data += data;
    } else {
      parsed.items.push_back(UndoItem{.item = item, .data = std::move(data)});
    }
  }
  return parsed;
}

// The action of `task`, or why the tray refuses to build it.
struct Built {
  std::unique_ptr<Action> action;
  const cleanup::HandlerCleanup* handlers = nullptr;  // set for a handler task
  const cleanup::RecycleBinCleanup* bin = nullptr;    // set for a recycle_bin task
};

Result<Built> build(const UserTask& task, const Toolbox& tools, const std::stop_token& stop) {
  switch (task.kind) {
    case TaskKind::native:
      if (cleanup::find_native_tool(task.command) == nullptr) {
        return fail(Errc::refused, "`" + task.command + "` is not an allowed native command");
      }
      return Built{.action = std::make_unique<cleanup::NativeCommand>(
                       *tools.runner, *tools.file_system, task.command, task.tier, stop)};
    case TaskKind::handler: {
      for (const std::string& name : task.handlers) {
        if (!user_handler(name)) {
          return fail(Errc::refused, "the handler \"" + name + "\" does not run in the tray");
        }
      }
      if (tools.handlers == nullptr || placement::drive_root(task.volume) != task.volume.native()) {
        return fail(Errc::invalid_argument, "not a volume root: " + safety::to_utf8(task.volume));
      }
      auto action = std::make_unique<cleanup::HandlerCleanup>(
          *tools.handlers, task.volume, task.handlers, task.tier, ExecutionContext::user, stop);
      const cleanup::HandlerCleanup* handlers = action.get();
      return Built{.action = std::move(action), .handlers = handlers};
    }
    case TaskKind::recycle_bin: {
      if (task.deleted_days < cleanup::min_bin_days) {
        return fail(Errc::refused, "the tray deletes no Recycle Bin item deleted less than " +
                                       std::to_string(cleanup::min_bin_days) + " days ago");
      }
      if (tools.recycle_bin == nullptr || tools.clock == nullptr) {
        return fail(Errc::invalid_state, "no Recycle Bin to clean");
      }
      auto action = std::make_unique<cleanup::RecycleBinCleanup>(
          *tools.recycle_bin, *tools.clock, task.deleted_days, task.tier, stop);
      const cleanup::RecycleBinCleanup* bin = action.get();
      return Built{.action = std::move(action), .bin = bin};
    }
    case TaskKind::relocate:
      if (task.recipe) {
        return Built{.action = std::make_unique<placement::RecipeAction>(
                         tools.placement, *tools.runner, *task.recipe, stop)};
      }
      break;
  }
  return Built{.action =
                   std::make_unique<placement::RelocateAction>(tools.placement, task.relocation)};
}

// The preview items to apply, under their plan item ids. A native command acts on the tool's
// whole cache and a relocation on the one folder, so they run once any item is left, under that
// item's id (by path when it matches); handlers and the Recycle Bin items run only when their own
// item is left.
Result<Preview> select(const UserTask& task, const Preview& preview) {
  if (task.items.empty()) {
    return fail(Errc::refused, "no item of the plan was approved");
  }
  Preview selected;
  for (PlanItem item : preview.items) {
    const auto match = std::ranges::find_if(task.items, [&](const TaskItem& approved) {
      return placement::same_path(approved.path, item.path);
    });
    if (match == task.items.end() &&
        (task.kind == TaskKind::handler || task.kind == TaskKind::recycle_bin)) {
      continue;
    }
    item.id = PlanItemId{match == task.items.end() ? task.items.front().id : match->id};
    selected.items.push_back(std::move(item));
  }
  return selected;
}

TaskReport failed(const UserTask& task, std::string error) {
  return TaskReport{.plan = task.plan,
                    .state = PlanState::failed,
                    .error = std::move(error),
                    .undo = {},
                    .verification = std::nullopt,
                    .skipped = {}};
}

TaskReport apply(const UserTask& task, Action& action, const std::stop_token& stop) {
  const Result<Preview> preview = action.preview();
  if (!preview) {
    return failed(task, preview.error().message);
  }
  const Result<Preview> selected = select(task, *preview);
  if (!selected) {
    return failed(task, selected.error().message);
  }
  if (stop.stop_requested()) {
    return failed(task, "cancelled: the tray is quitting");
  }
  Result<Undo> undo = action.apply(*selected);
  if (!undo) {
    return failed(task, undo.error().message);
  }
  TaskReport report{.plan = task.plan,
                    .state = PlanState::applied,
                    .error = {},
                    .undo = *std::move(undo),
                    .verification = std::nullopt,
                    .skipped = {}};
  Result<Verification> verification = action.verify(report.undo);
  if (!verification) {
    report.error = "applied, but not verified: " + verification.error().message;
    return report;
  }
  report.verification = *verification;
  if (verification->effect != Effect::worse) {
    report.state = PlanState::verified;
    return report;
  }
  report.error = "verification found " + verification->metric + " worse";
  if (report.undo.reversible()) {
    if (const Result<void> undone = action.undo(report.undo); undone) {
      report.state = PlanState::rolled_back;
      report.error += ", so it was undone";
    } else {
      report.error += ", and undoing it failed: " + undone.error().message;
    }
  }
  return report;
}

// The undo entries of `undo`, each item's data in pieces of at most the record's string size;
// nullopt when they need more entries than a record list holds.
std::optional<Json> undo_entries(const Undo& undo) {
  Json entries = Json::array();
  for (const UndoItem& item : undo.items) {
    std::string_view data = item.data;
    // One entry at least, so that empty data is stored too.
    for (bool first = true; first || !data.empty(); first = false) {
      if (entries.size() == ipc::max_record_elements) {
        return std::nullopt;
      }
      const std::size_t piece = cut(data, ipc::max_record_string);
      entries.push_back(Json{{"item", item.item.value()}, {"data", data.substr(0, piece)}});
      data.remove_prefix(piece);
    }
  }
  return entries;
}

}  // namespace

bool user_handler(std::string_view name) {
  return std::ranges::find(user_handlers, name) != user_handlers.end();
}

Result<std::int64_t> task_plan(const Json& task) {
  const auto found = task.is_object() ? task.find("plan") : task.end();
  if (found == task.end() || !found->is_number_integer()) {
    return fail(Errc::invalid_data, "user_tasks.next: a task without a plan id");
  }
  return found->get<std::int64_t>();
}

Result<UserTask> parse_task(const Json& task) {
  try {
    UserTask parsed;
    parsed.plan = task.at("plan").get<std::int64_t>();
    parsed.title = task.at("title").get<std::string>();
    const std::string kind = task.at("kind").get<std::string>();
    const std::optional<TaskKind> known = parse_kind(kind);
    if (!known) {
      return fail(Errc::refused, "the tray does not run plans of kind \"" + kind + "\"");
    }
    parsed.kind = *known;
    parsed.operation = parse_text(task, "operation", &parse_operation);
    parsed.tier = parse_text(task, "tier", &parse_tier);
    for (const Json& item : task.at("items")) {
      if (item.at("approved") != false) {
        parsed.items.push_back(TaskItem{.id = item.at("id").get<std::int64_t>(),
                                        .path = path_of(item.at("path")),
                                        .bytes = Bytes{item.at("bytes").get<std::uint64_t>()}});
      }
    }
    parse_action(task.at("action"), parsed);
    if (parsed.operation == Operation::undo) {
      parsed.undo = parse_undo(task.at("undo"));
    }
    return parsed;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("user_tasks.next: ") + error.what());
  }
}

Json report_record(const TaskReport& report) {
  Json record{{"state", to_string(report.state)},
              {"error", report.error.empty() ? Json(nullptr) : Json(bounded(report.error))}};
  std::string not_reversible = report.undo.not_reversible;
  if (!report.undo.items.empty()) {
    if (std::optional<Json> entries = undo_entries(report.undo)) {
      record["undo"] = *std::move(entries);
    } else {
      not_reversible = "the undo data is too large to store";
    }
  }
  if (!not_reversible.empty()) {
    record["not_reversible"] = bounded(not_reversible);
  }
  if (report.verification) {
    record["verification"] = Json{{"metric", bounded(report.verification->metric)},
                                  {"before", report.verification->before},
                                  {"after", report.verification->after},
                                  {"effect", to_string(report.verification->effect)}};
  }
  if (!report.skipped.empty()) {
    Json skipped = Json::array();
    for (const cleanup::Skipped& entry : report.skipped) {
      if (skipped.size() == ipc::max_record_elements) {
        break;
      }
      skipped.push_back(Json{{"target", bounded(entry.target)},
                             {"code", to_string(entry.error.code)},
                             {"message", bounded(entry.error.message)}});
    }
    record["skipped"] = std::move(skipped);
  }
  return record;
}

std::string summary(const TaskReport& report, std::string_view title) {
  std::string line = "Plan " + std::to_string(report.plan);
  if (!title.empty()) {
    line += " (" + std::string(title) + ")";
  }
  line += ": " + std::string(to_string(report.state));
  if (!report.error.empty()) {
    line += " - " + report.error;
  }
  for (const cleanup::Skipped& skipped : report.skipped) {
    line += "; " + skipped.target + " left out: " + skipped.error.message;
  }
  return line;
}

TaskReport run(const UserTask& task, const Toolbox& tools, const std::stop_token& stop) {
  Result<Built> built = build(task, tools, stop);
  if (!built) {
    return failed(task, built.error().message);
  }
  TaskReport report;
  if (task.operation == Operation::undo) {
    const Result<void> undone = built->action->undo(task.undo);
    report = undone ? TaskReport{.plan = task.plan,
                                 .state = PlanState::rolled_back,
                                 .error = {},
                                 .undo = {},
                                 .verification = std::nullopt,
                                 .skipped = {}}
                    : failed(task, undone.error().message);
  } else {
    report = apply(task, *built->action, stop);
  }
  if (built->handlers != nullptr) {
    report.skipped = built->handlers->skipped();
  }
  if (built->bin != nullptr) {
    report.skipped = built->bin->skipped();
  }
  return report;
}

}  // namespace myclean::user_tasks
