#include "core/cleanup/native_command.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/actions/item_action.hpp"
#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/support.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/paths.hpp"

namespace myclean::cleanup {

namespace {

namespace fs = std::filesystem;

// The commands of F-013, with the flags that keep them from asking for input.
constexpr std::array<NativeTool, 8> tools{{
    {.command = "pnpm store prune",
     .program = "pnpm",
     .prune = "store prune",
     .locate = "store path"},
    {.command = "npm cache verify",
     .program = "npm",
     .prune = "cache verify",
     .locate = "config get cache"},
    {.command = "yarn cache clean",
     .program = "yarn",
     .prune = "cache clean",
     .locate = "cache dir"},
    {.command = "pip cache purge", .program = "pip", .prune = "cache purge", .locate = "cache dir"},
    {.command = "uv cache prune", .program = "uv", .prune = "cache prune", .locate = "cache dir"},
    {.command = "dotnet nuget locals all --clear",
     .program = "dotnet",
     .prune = "nuget locals all --clear",
     .locate = "nuget locals all --list"},
    {.command = "go clean -cache -modcache",
     .program = "go",
     .prune = "clean -cache -modcache",
     .locate = "env GOCACHE GOMODCACHE"},
    {.command = "docker system prune",
     .program = "docker",
     .prune = "system prune --force",
     .locate = "system df --format {{.Type}}={{.Reclaimable}}",
     .report = CacheReport::docker},
}};

constexpr std::uint64_t docker_unit = 1000;  // docker prints decimal units ("1.2GB", "512kB")

// Each distinct absolute path in `out`, taken from a whole line or from after "<label>: ".
std::vector<fs::path> cache_paths(std::string_view out) {
  std::vector<fs::path> found;
  for (std::string_view line : lines(out)) {
    if (const std::size_t label = line.find(": "); label != std::string_view::npos) {
      line = trim(line.substr(label + 2));
    }
    fs::path path = from_utf8(line);
    if (path.is_absolute() && std::ranges::find(found, path) == found.end()) {
      found.push_back(std::move(path));
    }
  }
  return found;
}

// The types `docker system prune` acts on, with their reclaimable bytes. Volumes are pruned
// only with --volumes, so they are left out.
Result<std::vector<PlanItem>> docker_items(std::string_view out) {
  std::vector<PlanItem> items;
  for (const std::string_view line : lines(out)) {
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return fail(Errc::invalid_data, "unexpected docker system df line: " + std::string(line));
    }
    const std::string_view type = line.substr(0, equals);
    const std::string_view value = line.substr(equals + 1);
    const std::optional<Bytes> size = parse_size(value.substr(0, value.find(" (")), docker_unit);
    if (!size) {
      return fail(Errc::invalid_data, "unexpected docker size: " + std::string(line));
    }
    if (type != "Local Volumes" && *size != Bytes{}) {
      items.push_back(PlanItem{.path = from_utf8("docker:" + std::string(type)), .size = *size});
    }
  }
  return items;
}

}  // namespace

const NativeTool* find_native_tool(std::string_view command) {
  std::string normalized;
  for (const std::string& word : words(command)) {
    normalized += (normalized.empty() ? "" : " ") + word;
  }
  const auto found = std::ranges::find(tools, std::string_view{normalized}, &NativeTool::command);
  return found == tools.end() ? nullptr : &*found;
}

NativeCommand::NativeCommand(ICommandRunner& runner, const os::IFileSystem& file_system,
                             std::string command, Tier tier, std::stop_token stop)
    : runner_(&runner),
      file_system_(&file_system),
      command_(std::move(command)),
      tier_(tier),
      stop_(std::move(stop)) {}

ActionInfo NativeCommand::describe() const {
  return {
      .what = "Run `" + command_ + "`",
      .why = "the tool prunes its own cache; deleting files underneath it would break its index",
      .risk = Risk::low,
      .tier = tier_,
      .context = ExecutionContext::user};
}

Result<const NativeTool*> NativeCommand::allowed_tool() const {
  const NativeTool* tool = find_native_tool(command_);
  if (tool == nullptr) {
    return fail(Errc::refused, "`" + command_ + "` is not an allowed native prune command");
  }
  if (!runner_->user_context()) {
    return fail(Errc::refused,
                "not executed: the user context is unavailable (native commands never run as a "
                "service account)");
  }
  return tool;
}

Result<CommandOutput> NativeCommand::run(const NativeTool& tool, std::string_view arguments,
                                         std::chrono::milliseconds timeout) const {
  const std::string shown = std::string(tool.program) + " " + std::string(arguments);
  Result<CommandOutput> output = runner_->run(
      Command{
          .program = std::string(tool.program), .arguments = words(arguments), .timeout = timeout},
      stop_);
  if (!output) {
    return fail(output.error().code, "`" + shown + "`: " + output.error().message);
  }
  if (output->exit_code != 0) {
    const std::vector<std::string_view> said =
        lines(output->err.empty() ? output->out : output->err);
    return fail(Errc::io_error,
                std::format("`{}` exited with code {}{}", shown, output->exit_code,
                            said.empty() ? std::string{} : ": " + std::string(said.front())));
  }
  return output;
}

Result<Preview> NativeCommand::preview() {
  const Result<const NativeTool*> tool = allowed_tool();
  if (!tool) {
    return tl::unexpected(tool.error());
  }
  const Result<CommandOutput> output = run(**tool, (*tool)->locate, locate_timeout);
  if (!output) {
    return tl::unexpected(output.error());
  }
  Preview preview;
  if ((*tool)->report == CacheReport::docker) {
    Result<std::vector<PlanItem>> items = docker_items(output->out);
    if (!items) {
      return tl::unexpected(items.error());
    }
    preview.items = *std::move(items);
    return preview;
  }
  const std::vector<fs::path> folders = cache_paths(output->out);
  if (folders.empty()) {
    return fail(Errc::invalid_data,
                std::format("`{} {}` reported no cache folder", (*tool)->program, (*tool)->locate));
  }
  for (const fs::path& folder : folders) {
    const Result<Bytes> size = actions::measure(*file_system_, folder);
    if (!size) {
      return tl::unexpected(size.error());
    }
    preview.items.push_back(PlanItem{.path = folder, .size = *size});
  }
  return preview;
}

Result<Undo> NativeCommand::apply(const Preview& preview) {
  const Result<const NativeTool*> tool = allowed_tool();
  if (!tool) {
    return tl::unexpected(tool.error());
  }
  Undo undo{.not_reversible = "regenerates"};
  before_ = Bytes{};
  if (preview.items.empty()) {
    return undo;
  }
  if (const Result<CommandOutput> output = run(**tool, (*tool)->prune, prune_timeout); !output) {
    return tl::unexpected(output.error());
  }
  before_ = preview.total_size();
  for (const PlanItem& item : preview.items) {
    undo.items.push_back({.item = item.id, .data = safety::to_utf8(item.path)});
  }
  return undo;
}

Result<Verification> NativeCommand::verify(const Undo& /*undo*/) {
  const Result<Preview> now = preview();
  if (!now) {
    return tl::unexpected(now.error());
  }
  return compare("cache_bytes", before_, now->total_size());
}

Result<void> NativeCommand::undo(const Undo& undo) {
  return fail(Errc::not_reversible, "not reversible: " + undo.not_reversible);
}

}  // namespace myclean::cleanup
