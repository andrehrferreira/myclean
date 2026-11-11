#include "core/user_tasks/uninstall.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/scanall/owner_decisions.hpp"

namespace myclean::user_tasks {

namespace {

using Json = nlohmann::json;

std::filesystem::path from_utf8(std::string_view text) {
  return std::filesystem::path{std::u8string(text.begin(), text.end())};
}

std::string_view trim_start(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  return text;
}

bool ends_exe(std::string_view text, std::size_t at) {
  constexpr std::string_view exe = ".exe";
  if (at + exe.size() > text.size()) {
    return false;
  }
  for (std::size_t index = 0; index < exe.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(text[at + index])) != exe[index]) {
      return false;
    }
  }
  const std::size_t end = at + exe.size();
  return end == text.size() || text[end] == ' ' || text[end] == '\t';
}

// The InstallLocation `undo` records; empty when it records none.
std::filesystem::path location_of(const Undo& undo) {
  if (undo.items.empty()) {
    return {};
  }
  const Json json = Json::parse(undo.items.front().data, nullptr, false);
  return json.is_object() ? from_utf8(json.value("location", std::string{}))
                          : std::filesystem::path{};
}

// The command `undo` records; the launched UninstallString.
std::string launched_of(const Undo& undo) {
  if (undo.items.empty()) {
    return {};
  }
  const Json json = Json::parse(undo.items.front().data, nullptr, false);
  return json.is_object() ? json.value("command", std::string{}) : std::string{};
}

}  // namespace

Result<CommandLine> split_command(std::string_view text) {
  text = trim_start(text);
  if (text.empty()) {
    return fail(Errc::invalid_argument, "an empty UninstallString");
  }
  if (text.front() == '"') {
    const std::size_t close = text.find('"', 1);
    if (close == std::string_view::npos || close == 1) {
      return fail(Errc::invalid_argument, "an UninstallString with an unclosed or empty quote");
    }
    return CommandLine{.program = from_utf8(text.substr(1, close - 1)),
                       .arguments = std::string(trim_start(text.substr(close + 1)))};
  }
  for (std::size_t at = text.find('.'); at != std::string_view::npos; at = text.find('.', at + 1)) {
    if (ends_exe(text, at)) {
      return CommandLine{.program = from_utf8(text.substr(0, at + 4)),
                         .arguments = std::string(trim_start(text.substr(at + 4)))};
    }
  }
  const std::size_t space = std::min(text.find(' '), text.size());
  return CommandLine{.program = from_utf8(text.substr(0, space)),
                     .arguments = std::string(trim_start(text.substr(space)))};
}

ActionInfo UninstallAction::describe() const {
  return {.what = "Uninstall " + name_,
          .why = "the owner chose to uninstall it; the program's own uninstaller runs",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::user};
}

Result<Preview> UninstallAction::preview() {
  Result<std::optional<scanall::UninstallEntry>> entry = scanall::uninstall_entry(*registry_, key_);
  if (!entry) {
    return tl::unexpected(entry.error());
  }
  const std::optional<scanall::UninstallEntry>& program = *entry;
  if (!program) {
    return fail(Errc::not_found, name_ + " is not installed any more");
  }
  return Preview{
      .items = {PlanItem{.path = from_utf8("uninstall:" + name_), .size = program->estimated}}};
}

Result<Undo> UninstallAction::apply(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "an uninstall preview holds exactly one item");
  }
  Result<std::optional<scanall::UninstallEntry>> entry = scanall::uninstall_entry(*registry_, key_);
  if (!entry) {
    return tl::unexpected(entry.error());
  }
  if (!*entry) {
    return fail(Errc::not_found, name_ + " is not installed any more");
  }
  const scanall::UninstallEntry& program = **entry;
  if (program.name != name_) {
    return fail(Errc::refused, "the Uninstall entry " + key_ + " now names \"" + program.name +
                                   "\", not \"" + name_ + "\"");
  }
  Result<CommandLine> command = split_command(program.uninstall);
  if (!command) {
    return tl::unexpected(command.error());
  }
  if (Result<void> launched =
          launcher_->launch(command->program, command->arguments, uninstall_wait, stop_);
      !launched) {
    return fail(launched.error().code,
                "starting `" + program.uninstall + "` failed: " + launched.error().message);
  }
  launched_ = program.uninstall;
  return Undo{.items = {UndoItem{.item = preview.items.front().id,
                                 .data = Json{{"key", key_},
                                              {"name", name_},
                                              {"command", program.uninstall},
                                              {"location", utf8_text(program.location)}}
                                             .dump()}},
              .not_reversible = "an uninstalled program comes back only with its installer"};
}

Result<Verification> UninstallAction::verify(const Undo& undo) {
  Result<std::optional<scanall::UninstallEntry>> entry = scanall::uninstall_entry(*registry_, key_);
  if (!entry) {
    return tl::unexpected(entry.error());
  }
  if (*entry) {
    const std::string command = launched_.empty() ? launched_of(undo) : launched_;
    return fail(Errc::invalid_state, "launched `" + command + "`; finish in the uninstaller (" +
                                         name_ + " is still installed)");
  }
  if (const std::filesystem::path folder = location_of(undo);
      files_ != nullptr && !folder.empty() && files_->stat(folder).has_value()) {
    return fail(Errc::invalid_state, name_ + " is uninstalled, but its folder " +
                                         utf8_text(folder) +
                                         " was left behind; MyClean does not delete it");
  }
  return Verification{
      .metric = "uninstall_entry", .before = 1.0, .after = 0.0, .effect = Effect::better};
}

Result<void> UninstallAction::undo(const Undo& /*undo*/) {
  return fail(Errc::not_reversible,
              "an uninstalled program comes back only with its installer: install it again");
}

}  // namespace myclean::user_tasks
