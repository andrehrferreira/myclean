#include "core/user_tasks/appx.hpp"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"

namespace myclean::user_tasks {

namespace {

constexpr std::string_view not_reversible =
    "a removed app comes back only from the Microsoft Store or its installer";

std::filesystem::path item_path(const std::string& name) {
  const std::string text = "uninstall:" + name;
  return std::filesystem::path{std::u8string(text.begin(), text.end())};
}

}  // namespace

ActionInfo AppxRemoval::describe() const {
  return {.what = "Uninstall " + name_,
          .why = "the owner approved removing it; Windows removes the app package",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::user};
}

Result<Preview> AppxRemoval::preview() {
  Result<bool> installed = remover_->installed(package_);
  if (!installed) {
    return tl::unexpected(installed.error());
  }
  if (!*installed) {
    return fail(Errc::not_found, name_ + " is not installed any more");
  }
  return Preview{.items = {PlanItem{.path = item_path(name_)}}};
}

Result<Undo> AppxRemoval::apply(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "an uninstall preview holds exactly one item");
  }
  Result<bool> installed = remover_->installed(package_);
  if (!installed) {
    return tl::unexpected(installed.error());
  }
  if (!*installed) {
    return fail(Errc::not_found, name_ + " is not installed any more");
  }
  if (Result<void> removed = remover_->remove(package_, stop_); !removed) {
    return fail(removed.error().code,
                "removing the package " + package_ + " failed: " + removed.error().message);
  }
  return Undo{
      .items = {UndoItem{.item = preview.items.front().id,
                         .data = nlohmann::json{{"package", package_}, {"name", name_}}.dump()}},
      .not_reversible = std::string(not_reversible)};
}

Result<Verification> AppxRemoval::verify(const Undo& /*undo*/) {
  Result<bool> installed = remover_->installed(package_);
  if (!installed) {
    return tl::unexpected(installed.error());
  }
  if (*installed) {
    return fail(Errc::invalid_state, name_ + " is still installed");
  }
  return Verification{
      .metric = "uninstall_entry", .before = 1.0, .after = 0.0, .effect = Effect::better};
}

Result<void> AppxRemoval::undo(const Undo& /*undo*/) {
  return fail(Errc::not_reversible, std::string(not_reversible) + ": install it again");
}

}  // namespace myclean::user_tasks
