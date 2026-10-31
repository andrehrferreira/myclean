#include "core/placement/system_space.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/safety/paths.hpp"

namespace myclean::placement {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::uint32_t default_full_percent = 40;
constexpr std::uint32_t reduced_percent = 20;
constexpr std::uint32_t system_page_file_mb = 4096;
constexpr std::uint64_t mebibyte = 1ULL << 20U;
constexpr std::uint64_t gibibyte = 1ULL << 30U;

std::string utf8(const fs::path& path) {
  return safety::to_utf8(path);
}

fs::path from_utf8(std::string_view text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

std::string gigabytes(Bytes bytes) {
  return std::format("{:.1f} GB", static_cast<double>(bytes.count()) / gibibyte);
}

std::string trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

Command powercfg(std::vector<std::string> arguments) {
  return Command{.tool = "powercfg.exe", .arguments = std::move(arguments)};
}

std::vector<std::string> texts(const std::vector<Command>& commands) {
  std::vector<std::string> result;
  result.reserve(commands.size());
  for (const Command& command : commands) {
    result.push_back(command.text());
  }
  return result;
}

// Runs `commands` in order; the first that cannot start or exits non-zero stops the rest.
Result<void> run_all(IProcessRunner& runner, std::span<const Command> commands) {
  for (const Command& command : commands) {
    Result<CommandResult> result = runner.run(command);
    if (!result) {
      return fail(result.error().code, command.text() + ": " + result.error().message);
    }
    if (result->exit_code != 0) {
      return fail(Errc::io_error, std::format("{} exited with {}: {}", command.text(),
                                              result->exit_code, trim(result->output)));
    }
  }
  return {};
}

// `read(json)` over the single undo item, with malformed data reported as invalid_data.
template <typename T, typename Read>
Result<T> read_undo(const Undo& undo, std::string_view what, Read read) {
  if (undo.items.size() != 1) {
    return fail(Errc::invalid_data, std::string(what) + " undo data must hold exactly one item");
  }
  try {
    return read(Json::parse(undo.items.front().data));
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string(what) + " undo data is malformed: " + error.what());
  }
}

Result<PlanItemId> single_item(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "the preview must hold exactly one item");
  }
  return preview.items.front().id;
}

Effect smaller_is_better(double before, double after) {
  if (after < before) {
    return Effect::better;
  }
  return after > before ? Effect::worse : Effect::unchanged;
}

struct Hibernation {
  HibernationMode mode_before = HibernationMode::full;
  std::uint32_t size_percent_before = 0;
  HibernationMode mode_after = HibernationMode::full;
  Bytes file_size_before;
};

Result<Hibernation> parse_hibernation(const Undo& undo) {
  return read_undo<Hibernation>(undo, "hibernation", [](const Json& json) -> Result<Hibernation> {
    const std::optional<HibernationMode> before =
        parse_hibernation_mode(json.at("mode_before").get<std::string>());
    const std::optional<HibernationMode> after =
        parse_hibernation_mode(json.at("mode_after").get<std::string>());
    if (!before || !after) {
      return fail(Errc::invalid_data, "hibernation undo data names an unknown mode");
    }
    return Hibernation{.mode_before = *before,
                       .size_percent_before = json.at("size_percent_before").get<std::uint32_t>(),
                       .mode_after = *after,
                       .file_size_before = Bytes{json.at("file_size_before").get<std::uint64_t>()}};
  });
}

std::string describe_config(const PageFileConfig& config) {
  if (config.automatic) {
    return "managed by Windows";
  }
  std::string text;
  for (const PageFile& file : config.files) {
    if (!text.empty()) {
      text += ", ";
    }
    text += utf8(file.path) + (file.maximum_mb == 0
                                   ? std::string(" system-managed size")
                                   : std::format(" {}-{} MB", file.initial_mb, file.maximum_mb));
  }
  return text.empty() ? "no page file" : text;
}

Json config_json(const PageFileConfig& config) {
  Json files = Json::array();
  for (const PageFile& file : config.files) {
    files.push_back(Json{{"path", utf8(file.path)},
                         {"initial_mb", file.initial_mb},
                         {"maximum_mb", file.maximum_mb}});
  }
  return Json{{"automatic", config.automatic}, {"files", std::move(files)}};
}

PageFileConfig config_from(const Json& json) {
  PageFileConfig config{.automatic = json.at("automatic").get<bool>()};
  for (const Json& file : json.at("files")) {
    config.files.push_back(PageFile{.path = from_utf8(file.at("path").get<std::string>()),
                                    .initial_mb = file.at("initial_mb").get<std::uint32_t>(),
                                    .maximum_mb = file.at("maximum_mb").get<std::uint32_t>()});
  }
  return config;
}

struct PageFileChange {
  PageFileConfig before;
  PageFileConfig after;
};

Result<PageFileChange> parse_page_files(const Undo& undo) {
  return read_undo<PageFileChange>(undo, "page file",
                                   [](const Json& json) -> Result<PageFileChange> {
                                     return PageFileChange{.before = config_from(json.at("before")),
                                                           .after = config_from(json.at("after"))};
                                   });
}

Bytes allocated(const PageFileState& state) {
  Bytes total;
  for (const PageFileUse& use : state.in_use) {
    total += use.allocated;
  }
  return total;
}

std::string describe_maximum(std::optional<Bytes> maximum) {
  return maximum ? gigabytes(*maximum) + " maximum" : std::string("unbounded");
}

double maximum_metric(std::optional<Bytes> maximum) {
  return maximum ? static_cast<double>(maximum->count()) : -1.0;
}

struct ShadowChange {
  std::optional<Bytes> before;
  std::optional<Bytes> after;
};

std::optional<Bytes> maximum_from(const Json& json) {
  return json.is_null() ? std::nullopt : std::optional<Bytes>{Bytes{json.get<std::uint64_t>()}};
}

Json maximum_json(std::optional<Bytes> maximum) {
  return maximum ? Json(maximum->count()) : Json(nullptr);
}

Result<ShadowChange> parse_shadow(const Undo& undo) {
  return read_undo<ShadowChange>(undo, "shadow storage",
                                 [](const Json& json) -> Result<ShadowChange> {
                                   return ShadowChange{.before = maximum_from(json.at("before")),
                                                       .after = maximum_from(json.at("after"))};
                                 });
}

}  // namespace

Bytes hibernation_size(Bytes memory, HibernationMode mode, std::uint32_t size_percent) {
  switch (mode) {
    case HibernationMode::off:
      return Bytes{};
    case HibernationMode::reduced:
      return Bytes{memory.count() / 100 * reduced_percent};
    case HibernationMode::full:
      return Bytes{memory.count() / 100 *
                   (size_percent == 0 ? default_full_percent : size_percent)};
  }
  return Bytes{};
}

std::vector<Command> hibernation_commands(const HibernationState& from, HibernationMode mode,
                                          std::uint32_t size_percent) {
  std::vector<Command> commands;
  if (mode == HibernationMode::off) {
    if (from.mode != HibernationMode::off) {
      commands.push_back(powercfg({"/h", "off"}));
    }
    return commands;
  }
  if (from.mode == HibernationMode::off) {
    commands.push_back(powercfg({"/h", "on"}));
  }
  if (from.mode != mode) {
    commands.push_back(powercfg({"/h", "/type", std::string(to_string(mode))}));
  }
  if (mode == HibernationMode::full && size_percent != 0 && size_percent != from.size_percent) {
    commands.push_back(powercfg({"/h", "/size", std::to_string(size_percent)}));
  }
  return commands;
}

std::optional<HibernationMode> recommended_hibernation(const HibernationState& state) {
  return state.mode == HibernationMode::full ? std::optional{HibernationMode::reduced}
                                             : std::nullopt;
}

Result<SettingSummary> HibernationAction::summary() const {
  Result<HibernationState> state = system_->hibernation();
  if (!state) {
    return tl::unexpected(state.error());
  }
  HibernationState after = *state;
  after.mode = mode_;
  const std::uint32_t percent = mode_ == HibernationMode::full ? state->size_percent : 0;
  return SettingSummary{
      .current = std::format("{} ({} {})", to_string(state->mode), utf8(state->file.filename()),
                             gigabytes(state->file_size)),
      .proposed = std::format("{} (about {})", to_string(mode_),
                              gigabytes(hibernation_size(state->memory, mode_, percent))),
      .cost = state->file_size,
      .commands = texts(hibernation_commands(*state, mode_, 0)),
      .undo_commands = texts(hibernation_commands(after, state->mode, state->size_percent))};
}

ActionInfo HibernationAction::describe() const {
  return {.what = "Set the hibernation type to " + std::string(to_string(mode_)),
          .why = mode_ == HibernationMode::off
                     ? "removes the hibernation file; hibernate and Fast Startup stop working"
                     : "the hibernation file is sized for this type; reduced keeps Fast Startup "
                       "and drops full hibernate",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::service};
}

Result<Preview> HibernationAction::preview() {
  Result<HibernationState> state = system_->hibernation();
  if (!state) {
    return tl::unexpected(state.error());
  }
  return Preview{.items = {PlanItem{.path = state->file, .size = state->file_size}}};
}

Result<Undo> HibernationAction::apply(const Preview& preview) {
  const Result<PlanItemId> item = single_item(preview);
  if (!item) {
    return tl::unexpected(item.error());
  }
  Result<HibernationState> before = system_->hibernation();
  if (!before) {
    return tl::unexpected(before.error());
  }
  if (before->mode == mode_) {
    return fail(Errc::invalid_state, "hibernation already is " + std::string(to_string(mode_)));
  }
  if (Result<void> ran = run_all(*runner_, hibernation_commands(*before, mode_, 0)); !ran) {
    // Reverse what did run: the commands from the state now back to the one before.
    if (Result<HibernationState> now = system_->hibernation(); now) {
      static_cast<void>(
          run_all(*runner_, hibernation_commands(*now, before->mode, before->size_percent)));
    }
    return tl::unexpected(ran.error());
  }
  const Json json{{"mode_before", to_string(before->mode)},
                  {"size_percent_before", before->size_percent},
                  {"mode_after", to_string(mode_)},
                  {"file_size_before", before->file_size.count()}};
  return Undo{.items = {UndoItem{.item = *item, .data = json.dump()}}};
}

Result<Verification> HibernationAction::verify(const Undo& undo) {
  Result<Hibernation> record = parse_hibernation(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  Result<HibernationState> now = system_->hibernation();
  if (!now) {
    return tl::unexpected(now.error());
  }
  const auto before = static_cast<double>(record->file_size_before.count());
  const auto after = static_cast<double>(now->file_size.count());
  return Verification{.metric = "hiberfil_bytes",
                      .before = before,
                      .after = after,
                      .effect = smaller_is_better(before, after)};
}

Result<void> HibernationAction::undo(const Undo& undo) {
  Result<Hibernation> record = parse_hibernation(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  Result<HibernationState> now = system_->hibernation();
  if (!now) {
    return tl::unexpected(now.error());
  }
  if (now->mode != record->mode_after) {
    return fail(Errc::invalid_state, "hibernation was changed to " +
                                         std::string(to_string(now->mode)) +
                                         " since; nothing was undone");
  }
  return run_all(*runner_,
                 hibernation_commands(*now, record->mode_before, record->size_percent_before));
}

std::vector<std::string> page_file_steps(const PageFileConfig& from, const PageFileConfig& to) {
  std::vector<std::string> steps;
  if (from.automatic && !to.automatic) {
    steps.emplace_back("set Win32_ComputerSystem AutomaticManagedPagefile=False");
  }
  const auto find = [](const PageFileConfig& config, const fs::path& path) {
    return std::ranges::find_if(config.files,
                                [&](const PageFile& file) { return same_path(file.path, path); });
  };
  for (const PageFile& file : from.files) {
    if (find(to, file.path) == to.files.end()) {
      steps.push_back(std::format("delete Win32_PageFileSetting Name='{}'", utf8(file.path)));
    }
  }
  for (const PageFile& file : to.files) {
    if (const auto same = find(from, file.path); same == from.files.end() || *same != file) {
      steps.push_back(
          std::format("set Win32_PageFileSetting Name='{}' InitialSize={} MaximumSize={}",
                      utf8(file.path), file.initial_mb, file.maximum_mb));
    }
  }
  if (to.automatic && !from.automatic) {
    steps.emplace_back("set Win32_ComputerSystem AutomaticManagedPagefile=True");
  }
  return steps;
}

std::optional<PageFileConfig> recommended_page_files(const PageFileState& state,
                                                     std::span<const TargetVolume> volumes,
                                                     const fs::path& system_root) {
  const std::wstring system = drive_root(system_root);
  const bool capped =
      !state.config.automatic && std::ranges::all_of(state.config.files, [&](const PageFile& file) {
        return drive_root(file.path) != system ||
               (file.maximum_mb > 0 && file.maximum_mb <= system_page_file_mb);
      });
  if (capped) {
    return std::nullopt;
  }
  const TargetVolume* best = best_target(volumes, system_root, allocated(state));
  if (best == nullptr) {
    return std::nullopt;
  }
  return PageFileConfig{.automatic = false,
                        .files = {PageFile{.path = fs::path{system} / "pagefile.sys",
                                           .initial_mb = system_page_file_mb,
                                           .maximum_mb = system_page_file_mb},
                                  PageFile{.path = best->root / "pagefile.sys"}}};
}

Result<SettingSummary> PageFileAction::summary() const {
  Result<PageFileState> state = system_->page_files();
  if (!state) {
    return tl::unexpected(state.error());
  }
  return SettingSummary{.current = describe_config(state->config),
                        .proposed = describe_config(config_),
                        .cost = allocated(*state),
                        .commands = page_file_steps(state->config, config_),
                        .undo_commands = page_file_steps(config_, state->config),
                        .restart_required = true};
}

ActionInfo PageFileAction::describe() const {
  return {.what = "Set the page files to: " + describe_config(config_),
          .why =
              "the page file takes space on the system drive; the change takes effect after a "
              "restart",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::service};
}

Result<Preview> PageFileAction::preview() {
  Result<PageFileState> state = system_->page_files();
  if (!state) {
    return tl::unexpected(state.error());
  }
  Preview preview;
  for (const PageFileUse& use : state->in_use) {
    preview.items.push_back(PlanItem{.path = use.path, .size = use.allocated});
  }
  return preview;
}

Result<Undo> PageFileAction::apply(const Preview& preview) {
  if (config_.automatic != config_.files.empty()) {
    return fail(Errc::invalid_argument,
                "a page-file configuration is either managed by Windows or lists its files");
  }
  Result<PageFileState> state = system_->page_files();
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (state->config == config_) {
    return fail(Errc::invalid_state, "the page files already are: " + describe_config(config_));
  }
  Result<std::vector<TargetVolume>> volumes = drives_->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  for (const PageFile& file : config_.files) {
    const TargetVolume* volume = volume_of(*volumes, file.path);
    if (volume == nullptr) {
      return fail(Errc::refused, "no mounted volume holds " + utf8(file.path));
    }
    // A drive that holds a page file now already has its room; any other must have it free.
    const bool holds_one = std::ranges::any_of(state->in_use, [&](const PageFileUse& use) {
      return drive_root(use.path) == drive_root(file.path);
    });
    const Bytes needed =
        file.maximum_mb > 0 ? Bytes{file.maximum_mb * mebibyte} : allocated(*state);
    if (const std::string why = unsuitable(*volume, needed);
        !why.empty() && (!volume->healthy || !holds_one)) {
      return fail(Errc::refused, utf8(volume->root) + " cannot hold a page file: " + why);
    }
  }
  if (Result<void> written = system_->set_page_files(config_); !written) {
    return tl::unexpected(written.error());
  }
  const Json json{{"before", config_json(state->config)}, {"after", config_json(config_)}};
  const PlanItemId item = preview.items.empty() ? PlanItemId{} : preview.items.front().id;
  return Undo{.items = {UndoItem{.item = item, .data = json.dump()}}};
}

Result<Verification> PageFileAction::verify(const Undo& undo) {
  Result<PageFileChange> change = parse_page_files(undo);
  if (!change) {
    return tl::unexpected(change.error());
  }
  Result<PageFileState> now = system_->page_files();
  if (!now) {
    return tl::unexpected(now.error());
  }
  const bool applied = now->config == change->after;
  return Verification{.metric = "page_file_settings_applied",
                      .before = 0.0,
                      .after = applied ? 1.0 : 0.0,
                      .effect = applied ? Effect::better : Effect::worse};
}

Result<void> PageFileAction::undo(const Undo& undo) {
  Result<PageFileChange> change = parse_page_files(undo);
  if (!change) {
    return tl::unexpected(change.error());
  }
  Result<PageFileState> now = system_->page_files();
  if (!now) {
    return tl::unexpected(now.error());
  }
  if (now->config != change->after) {
    return fail(Errc::invalid_state, "the page files were changed since (now " +
                                         describe_config(now->config) + "); nothing was undone");
  }
  return system_->set_page_files(change->before);
}

Command shadow_storage_command(const ShadowStorage& storage, std::optional<Bytes> maximum) {
  return Command{
      .tool = "vssadmin.exe",
      .arguments = {
          "resize", "shadowstorage", "/for=" + storage.volume, "/on=" + storage.diff_volume,
          "/maxsize=" + (maximum ? std::to_string(maximum->count()) : std::string("UNBOUNDED"))}};
}

std::optional<Bytes> recommended_shadow_maximum(const ShadowStorage& storage, Bytes volume_size) {
  const Bytes target{std::max<std::uint64_t>(10 * gibibyte, volume_size.count() / 100 * 5)};
  if (storage.maximum && *storage.maximum <= target) {
    return std::nullopt;
  }
  return target;
}

Result<ShadowStorage> ShadowStorageAction::storage() const {
  Result<std::vector<ShadowStorage>> all = system_->shadow_storage();
  if (!all) {
    return tl::unexpected(all.error());
  }
  const auto found = std::ranges::find_if(
      *all, [&](const ShadowStorage& storage) { return same_path(storage.volume, volume_); });
  if (found == all->end()) {
    return fail(Errc::not_found, "no shadow storage is set up for " + volume_);
  }
  return *found;
}

Result<SettingSummary> ShadowStorageAction::summary() const {
  Result<ShadowStorage> current = storage();
  if (!current) {
    return tl::unexpected(current.error());
  }
  return SettingSummary{
      .current = describe_maximum(current->maximum) + ", " + gigabytes(current->used) + " used",
      .proposed = describe_maximum(maximum_),
      .cost = current->allocated,
      .commands = {shadow_storage_command(*current, maximum_).text()},
      .undo_commands = {shadow_storage_command(*current, current->maximum).text()}};
}

ActionInfo ShadowStorageAction::describe() const {
  return {
      .what = "Limit the restore-point storage of " + volume_ + " to " + describe_maximum(maximum_),
      .why =
          "restore points take space on the drive; shrinking deletes the oldest ones that "
          "no longer fit, and undo restores the limit, not them",
      .risk = Risk::medium,
      .tier = Tier::ask,
      .context = ExecutionContext::service};
}

Result<Preview> ShadowStorageAction::preview() {
  Result<ShadowStorage> current = storage();
  if (!current) {
    return tl::unexpected(current.error());
  }
  std::string root = current->diff_volume;
  if (root.ends_with(':')) {
    root += '\\';  // "C:" alone names the current folder of drive C
  }
  return Preview{.items = {PlanItem{.path = from_utf8(root) / "System Volume Information",
                                    .size = current->allocated}}};
}

Result<Undo> ShadowStorageAction::apply(const Preview& preview) {
  const Result<PlanItemId> item = single_item(preview);
  if (!item) {
    return tl::unexpected(item.error());
  }
  if (maximum_ && *maximum_ < minimum_shadow_storage) {
    return fail(Errc::invalid_argument, "vssadmin needs a shadow storage of at least 320 MB");
  }
  Result<ShadowStorage> current = storage();
  if (!current) {
    return tl::unexpected(current.error());
  }
  if (current->maximum == maximum_) {
    return fail(Errc::invalid_state,
                "the shadow storage of " + volume_ + " already is " + describe_maximum(maximum_));
  }
  const Command command = shadow_storage_command(*current, maximum_);
  if (Result<void> ran = run_all(*runner_, std::span{&command, 1}); !ran) {
    return tl::unexpected(ran.error());
  }
  const Json json{{"before", maximum_json(current->maximum)}, {"after", maximum_json(maximum_)}};
  return Undo{.items = {UndoItem{.item = *item, .data = json.dump()}}};
}

Result<Verification> ShadowStorageAction::verify(const Undo& undo) {
  Result<ShadowChange> change = parse_shadow(undo);
  if (!change) {
    return tl::unexpected(change.error());
  }
  Result<ShadowStorage> now = storage();
  if (!now) {
    return tl::unexpected(now.error());
  }
  return Verification{.metric = "shadow_storage_maximum",
                      .before = maximum_metric(change->before),
                      .after = maximum_metric(now->maximum),
                      .effect = now->maximum == change->after ? Effect::better : Effect::worse};
}

Result<void> ShadowStorageAction::undo(const Undo& undo) {
  Result<ShadowChange> change = parse_shadow(undo);
  if (!change) {
    return tl::unexpected(change.error());
  }
  Result<ShadowStorage> now = storage();
  if (!now) {
    return tl::unexpected(now.error());
  }
  if (now->maximum != change->after) {
    return fail(Errc::invalid_state, "the shadow storage of " + volume_ + " was changed since (" +
                                         describe_maximum(now->maximum) + "); nothing was undone");
  }
  const Command command = shadow_storage_command(*now, change->before);
  return run_all(*runner_, std::span{&command, 1});
}

}  // namespace myclean::placement
