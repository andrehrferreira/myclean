#include "core/ipc/plans.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace myclean::ipc {

namespace {

using Json = nlohmann::json;

// The first member of `record` not in `known`, or "".
std::string unknown_member(const Json& record, std::initializer_list<std::string_view> known) {
  for (const auto& [name, value] : record.items()) {
    if (std::ranges::find(known, name) == known.end()) {
      return "unknown member '" + name + "'";
    }
  }
  return {};
}

// Why member `name` of `record`, if present, is not a list of integers.
std::string id_list(const Json& record, std::string_view name) {
  const auto list = record.find(name);
  if (list == record.end()) {
    return {};
  }
  if (!list->is_array() || !std::ranges::all_of(*list, &Json::is_number_integer)) {
    return "'" + std::string(name) + "' must be a list of integers";
  }
  return {};
}

std::string check_undo_entries(const Json& entries) {
  if (!entries.is_array()) {
    return "'undo' must be a list";
  }
  for (const Json& entry : entries) {
    if (!entry.is_object() || !entry.contains("item") || !entry.at("item").is_number_integer() ||
        !entry.contains("data") || !entry.at("data").is_string() || entry.size() != 2) {
      return R"(each 'undo' entry must be {"item": int, "data": string})";
    }
  }
  return {};
}

std::string check_skipped(const Json& entries) {
  if (!entries.is_array()) {
    return "'skipped' must be a list";
  }
  for (const Json& entry : entries) {
    if (!entry.is_object() || entry.size() != 3 ||
        !std::ranges::all_of(
            std::initializer_list<const char*>{"target", "code", "message"},
            [&](const char* name) { return entry.contains(name) && entry.at(name).is_string(); })) {
      return R"(each 'skipped' entry must be {"target", "code", "message"} strings)";
    }
  }
  return {};
}

std::string check_verification(const Json& verification) {
  if (!verification.is_object()) {
    return "'verification' must be an object";
  }
  if (const std::string unknown =
          unknown_member(verification, {"metric", "before", "after", "effect"});
      !unknown.empty()) {
    return "verification: " + unknown;
  }
  const auto effect = verification.find("effect");
  if (!verification.contains("metric") || !verification.at("metric").is_string() ||
      !verification.contains("before") || !verification.at("before").is_number() ||
      !verification.contains("after") || !verification.at("after").is_number() ||
      effect == verification.end() ||
      !(*effect == "better" || *effect == "unchanged" || *effect == "worse")) {
    return "verification needs metric, before, after and effect (better|unchanged|worse)";
  }
  return {};
}

}  // namespace

std::string check_selection(const Json& record) {
  if (std::string unknown = unknown_member(record, {"all", "items", "except"}); !unknown.empty()) {
    return unknown;
  }
  const auto all = record.find("all");
  if (all == record.end() || !all->is_boolean()) {
    return "'all' must be a boolean";
  }
  if (std::string problem = id_list(record, "items"); !problem.empty()) {
    return problem;
  }
  return id_list(record, "except");
}

std::string check_candidates(const Json& record) {
  if (std::string unknown = unknown_member(record, {"ids"}); !unknown.empty()) {
    return unknown;
  }
  const auto ids = record.find("ids");
  if (ids == record.end()) {
    return "'ids' is required";
  }
  if (std::string problem = id_list(record, "ids"); !problem.empty()) {
    return problem;
  }
  return ids->empty() ? "'ids' must name at least one candidate" : "";
}

std::string check_task_result(const Json& record) {
  if (std::string unknown = unknown_member(
          record, {"state", "error", "not_reversible", "undo", "verification", "skipped"});
      !unknown.empty()) {
    return unknown;
  }
  const auto state = record.find("state");
  if (state == record.end() || !(*state == "applied" || *state == "verified" ||
                                 *state == "rolled_back" || *state == "failed")) {
    return "'state' must be applied, verified, rolled_back or failed";
  }
  if (const auto error = record.find("error");
      error != record.end() && !error->is_string() && !error->is_null()) {
    return "'error' must be a string or null";
  }
  if (const auto text = record.find("not_reversible"); text != record.end() && !text->is_string()) {
    return "'not_reversible' must be a string";
  }
  if (const auto undo = record.find("undo"); undo != record.end()) {
    if (std::string problem = check_undo_entries(*undo); !problem.empty()) {
      return problem;
    }
  }
  if (const auto skipped = record.find("skipped"); skipped != record.end()) {
    if (std::string problem = check_skipped(*skipped); !problem.empty()) {
      return problem;
    }
  }
  if (const auto verification = record.find("verification");
      verification != record.end() && !verification->is_null()) {
    return check_verification(*verification);
  }
  return {};
}

}  // namespace myclean::ipc
