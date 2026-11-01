#include "core/cleanup/native_command.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
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
constexpr std::array<NativeTool, 9> tools{{
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
    // WSL 1 never empties /tmp. Runs as root inside each distribution with --exec, so no shell
    // reads the find expression: the entries of /tmp older than 7 days, the X11/ICE/font socket
    // folders (".X11-unix" and the like) kept. "-d <distribution>" goes in front.
    {.command = "wsl tmp prune",
     .program = "wsl",
     .prune = "-u root --exec find /tmp -mindepth 1 -maxdepth 1 -mtime +7 ! -name .*-unix -exec "
              "rm -rf {} +",
     .locate = "-l -q",
     .report = CacheReport::wsl_tmp},
}};

constexpr std::uint64_t docker_unit = 1000;  // docker prints decimal units ("1.2GB", "512kB")

// What /tmp takes on disk inside a distribution, in KiB (POSIX du, busybox's too).
constexpr std::string_view wsl_tmp_size = "-u root --exec du -sk /tmp";
constexpr std::string_view wsl_item_prefix = "wsl:";
constexpr std::string_view wsl_item_suffix = ":/tmp";
constexpr std::uint64_t kib = 1024;

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

// True for a name WSL accepts for a distribution: ASCII letters, digits, '.', '_' and '-'.
bool distribution_name(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x80 &&
           (std::isalnum(byte) != 0 || character == '.' || character == '_' || character == '-');
  });
}

bool docker_desktop(std::string_view name) {
  std::string lower(name);
  std::ranges::transform(lower, lower.begin(), [](char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  });
  return lower == "docker-desktop" || lower == "docker-desktop-data";
}

// The distributions `wsl -l -q` lists, but Docker Desktop's own. wsl.exe writes UTF-16LE, which
// the runner hands on unchanged (ASCII in UTF-16LE is valid UTF-8), or, after a byte order mark,
// as the OEM code page left it: the mark turns into non-ASCII bytes and the rest stays. A code
// unit beyond ASCII spoils its line, so each line is a name WSL accepts or is skipped: no name
// is ever guessed.
std::vector<std::string> wsl_distributions(std::string_view out) {
  std::string text;
  if (out.find('\0') == std::string_view::npos) {
    text = out;  // already UTF-8 (WSL_UTF8=1)
  } else {
    std::size_t at = 0;
    while (at < out.size() && static_cast<unsigned char>(out[at]) >= 0x80) {
      ++at;  // the byte order mark
    }
    for (; at + 1 < out.size(); at += 2) {
      const auto low = static_cast<unsigned char>(out[at]);
      text.push_back(out[at + 1] == '\0' && low < 0x80 ? static_cast<char>(low) : '?');
    }
  }
  std::vector<std::string> names;
  for (std::string_view line : lines(text)) {
    if (line.starts_with('*')) {
      line = trim(line.substr(1));  // the default distribution's mark in `wsl -l -v`
    }
    if (distribution_name(line) && !docker_desktop(line) &&
        std::ranges::find(names, line) == names.end()) {
      names.emplace_back(line);
    }
  }
  return names;
}

// The distribution of a preview item "wsl:<distribution>:/tmp"; nothing for any other item.
std::optional<std::string> distribution_of(const PlanItem& item) {
  const std::string text = safety::to_utf8(item.path);
  if (!text.starts_with(wsl_item_prefix) || !text.ends_with(wsl_item_suffix)) {
    return std::nullopt;
  }
  std::string name = text.substr(wsl_item_prefix.size(),
                                 text.size() - wsl_item_prefix.size() - wsl_item_suffix.size());
  if (!distribution_name(name)) {
    return std::nullopt;
  }
  return name;
}

// `arguments` run inside the distribution `name`.
std::string in_distribution(std::string_view name, std::string_view arguments) {
  return "-d " + std::string(name) + " " + std::string(arguments);
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
  if ((*tool)->report == CacheReport::wsl_tmp) {
    return wsl_preview(**tool, output->out);
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
  if ((*tool)->report == CacheReport::wsl_tmp) {
    if (const Result<void> pruned = wsl_prune(**tool, preview); !pruned) {
      return tl::unexpected(pruned.error());
    }
  } else if (const Result<CommandOutput> output = run(**tool, (*tool)->prune, prune_timeout);
             !output) {
    return tl::unexpected(output.error());
  }
  before_ = preview.total_size();
  for (const PlanItem& item : preview.items) {
    undo.items.push_back({.item = item.id, .data = safety::to_utf8(item.path)});
  }
  return undo;
}

Result<Preview> NativeCommand::wsl_preview(const NativeTool& tool, std::string_view listed) const {
  Preview preview;
  std::optional<Error> first_error;
  for (const std::string& name : wsl_distributions(listed)) {
    const Result<CommandOutput> output =
        run(tool, in_distribution(name, wsl_tmp_size), wsl_measure_timeout);
    if (!output) {
      first_error = first_error.value_or(output.error());
      continue;
    }
    const std::vector<std::string_view> said = lines(output->out);
    const std::string_view size =
        said.empty() ? std::string_view{} : said.front().substr(0, said.front().find('\t'));
    std::uint64_t size_kib = 0;
    const char* const end = std::to_address(size.end());
    if (size.empty() || std::from_chars(std::to_address(size.begin()), end, size_kib).ptr != end) {
      first_error = first_error.value_or(
          Error{.code = Errc::invalid_data,
                .message = std::format("unexpected du output in {}: {}", name,
                                       said.empty() ? std::string_view{} : said.front())});
      continue;
    }
    preview.items.push_back(PlanItem{
        .path = from_utf8(std::string(wsl_item_prefix) + name + std::string(wsl_item_suffix)),
        .size = Bytes{size_kib * kib}});
  }
  if (preview.items.empty() && first_error) {
    return tl::unexpected(*first_error);
  }
  return preview;
}

Result<void> NativeCommand::wsl_prune(const NativeTool& tool, const Preview& preview) const {
  std::optional<Error> first_error;
  for (const PlanItem& item : preview.items) {
    const std::optional<std::string> name = distribution_of(item);
    if (!name) {
      continue;
    }
    if (const Result<CommandOutput> output =
            run(tool, in_distribution(*name, tool.prune), wsl_prune_timeout);
        !output) {
      first_error = first_error.value_or(output.error());
    }
  }
  if (first_error) {
    return tl::unexpected(*first_error);
  }
  return {};
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
