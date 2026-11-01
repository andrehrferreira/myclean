#include "cli/scan_all_command.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <mutex>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "cli/cli.hpp"
#include "cli/map_command.hpp"
#include "cli/packs_command.hpp"
#include "core/clean/engine.hpp"
#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/walk.hpp"
#include "core/clean/workspaces.hpp"
#include "core/compress/analyzer.hpp"
#include "core/compress/compression.hpp"
#include "core/drives/analysis.hpp"
#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/reading.hpp"
#include "core/index/indexed_volume.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/mcp/client.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/drive_health.hpp"
#include "core/scanall/duplicates.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/placement.hpp"
#include "core/scanall/report.hpp"
#include "core/scanall/sources.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"
#include "core/telemetry/volume_info.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/compress/win32_compression.hpp"
#include "platform/defrag/system_sources.hpp"
#include "platform/defrag/volume_layout.hpp"
#include "platform/disk/win32_volume_info.hpp"
#include "platform/disk_health/win32_disk_health.hpp"
#include "platform/fs/protected_locations.hpp"
#include "platform/git/git_cli.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ntfs/win32_ntfs_volume.hpp"
#include "platform/scan/directory_reader.hpp"
#include "platform/scan/file_content.hpp"
#include "platform/settings/seek_penalty.hpp"

namespace myclean::cli {

namespace {

namespace fs = std::filesystem;
using scanall::DriveInfo;
using scanall::Finding;

constexpr Bytes min_finding{std::uint64_t{64} << 20U};
constexpr Bytes min_compress_folder{std::uint64_t{1} << 30U};
constexpr std::size_t max_compress_folders = 6;
constexpr std::chrono::milliseconds service_timeout{5000};

constexpr std::string_view usage_text =
    "usage: myclean scan-all [--drives C:,D:,...] [--budget-minutes <n>] [--data <dir>] [--json]\n"
    "  every fixed volume (or the ones listed), each tree stopped after <n> minutes (10);\n"
    "  data may be proposed to move to any fixed volume, listed or not\n";

struct Request {
  std::vector<std::wstring> drives;  // roots ("C:\"); empty: every fixed volume
  std::chrono::minutes budget{10};
  std::optional<fs::path> data_dir;
  bool json = false;
};

Result<std::wstring> drive_root_of(std::string_view text) {
  const bool letter =
      !text.empty() && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z'));
  const std::string_view rest = text.empty() ? text : text.substr(1);
  if (!letter || (!rest.empty() && rest != ":" && rest != ":\\")) {
    return fail(Errc::invalid_argument, "--drives: not a drive: " + std::string(text));
  }
  const char upper = text[0] >= 'a' ? static_cast<char>(text[0] - 'a' + 'A') : text[0];
  return std::wstring{static_cast<wchar_t>(upper), L':', L'\\'};
}

Result<void> parse_option(Request& request, std::string_view name, const std::string& value) {
  if (name == "--drives") {
    for (const auto part : std::views::split(value, ',')) {
      Result<std::wstring> root = drive_root_of(std::string_view(part.begin(), part.end()));
      if (!root) {
        return tl::unexpected(root.error());
      }
      request.drives.push_back(*std::move(root));
    }
  } else if (name == "--budget-minutes") {
    int minutes = 0;
    const char* const last = std::to_address(value.end());
    const auto [end, error] = std::from_chars(value.data(), last, minutes);
    if (error != std::errc{} || end != last || minutes < 1) {
      return fail(Errc::invalid_argument, "--budget-minutes: not a positive number: " + value);
    }
    request.budget = std::chrono::minutes{minutes};
  } else if (name == "--data") {
    request.data_dir = fs::path{std::u8string(value.begin(), value.end())};
  } else {
    return fail(Errc::invalid_argument, "unknown option " + std::string(name));
  }
  return {};
}

Result<Request> parse(std::span<const std::string> args) {
  Request request;
  for (std::size_t index = 0; index < args.size(); ++index) {
    if (args[index] == "--json") {
      request.json = true;
    } else if (index + 1 >= args.size()) {
      return fail(Errc::invalid_argument, "unknown or incomplete option " + args[index]);
    } else if (Result<void> parsed = parse_option(request, args[index], args[index + 1]); !parsed) {
      return tl::unexpected(parsed.error());
    } else {
      ++index;
    }
  }
  return request;
}

// Windows background mode (very low I/O and memory priority) and then below-normal CPU priority
// for this process while it lives: entering background mode resets the priority class, and a
// later class change keeps background mode.
class ProcessBackground {
 public:
  ProcessBackground()
      : previous_(::GetPriorityClass(::GetCurrentProcess())),
        background_(::SetPriorityClass(::GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN) !=
                    FALSE) {
    static_cast<void>(::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS));
  }
  ~ProcessBackground() {
    if (background_) {
      static_cast<void>(::SetPriorityClass(::GetCurrentProcess(), PROCESS_MODE_BACKGROUND_END));
    }
    if (previous_ != 0) {
      static_cast<void>(::SetPriorityClass(::GetCurrentProcess(), previous_));
    }
  }
  ProcessBackground(const ProcessBackground&) = delete;
  ProcessBackground& operator=(const ProcessBackground&) = delete;
  ProcessBackground(ProcessBackground&&) = delete;
  ProcessBackground& operator=(ProcessBackground&&) = delete;

 private:
  DWORD previous_;
  bool background_;
};

// A stop token that is requested once `budget` has passed.
class Deadline {
 public:
  explicit Deadline(std::chrono::steady_clock::duration budget)
      : timer_(
            [this, until = std::chrono::steady_clock::now() + budget](const std::stop_token& done) {
              std::mutex mutex;
              std::condition_variable_any wake;
              std::unique_lock lock(mutex);
              static_cast<void>(wake.wait_until(lock, done, until, [] { return false; }));
              if (!done.stop_requested()) {
                stop_.request_stop();
              }
            }) {}

  [[nodiscard]] std::stop_token token() const { return stop_.get_token(); }
  [[nodiscard]] bool expired() const { return stop_.stop_requested(); }

 private:
  std::stop_source stop_;
  std::jthread timer_;  // after stop_: started after it, joined before it goes
};

fs::path utf8_path(const std::string& text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// The data the analyses use: the knowledge packs that apply here and the workspace roots.
struct Knowledge {
  std::vector<map::Area> areas;
  std::vector<rules::CleaningRule> rules;
  std::vector<packs::CompressTarget> compress;
  std::optional<clean::Workspaces> workspaces;
};

Knowledge load_knowledge(const fs::path& data_dir, std::vector<std::string>& notes) {
  LocalPacks local = load_local_packs(data_dir);
  for (const Error& error : local.report.errors) {
    notes.push_back("packs: " + error.message);
  }
  Knowledge knowledge{.areas = std::move(local.registry.attribution),
                      .rules = std::move(local.registry.clean),
                      .compress = std::move(local.registry.compress)};
  const rules::DiskDataFiles files;
  const fs::path workspace_file = data_dir / "workspaces.toml";
  if (Result<std::string> text = files.read(workspace_file); text) {
    if (Result<clean::Workspaces> parsed = clean::parse_workspace_file(*text, workspace_file);
        parsed) {
      knowledge.workspaces = *std::move(parsed);
    } else {
      notes.push_back(parsed.error().message);
    }
  }
  return knowledge;
}

health::HealthView read_health(const fs::path& data_dir, WallTime now,
                               std::vector<std::string>& notes) {
  health::HealthView view{.time = now};
  const platform::Win32DiskHealthSource source;
  const Result<std::vector<health::DriveProbe>> probes = source.probe();
  if (!probes) {
    notes.push_back("disk health: " + probes.error().message);
    return view;
  }
  const rules::DiskDataFiles files;
  const Result<health::DriveSpecs> specs = health::DriveSpecs::load(files, data_dir);
  for (const health::DriveProbe& probe : *probes) {
    health::HealthReading reading = health::interpret(probe);
    health::Assessment assessment =
        health::assess(reading, specs ? specs->find(reading.model) : nullptr);
    view.drives.push_back(
        health::DriveHealth{.reading = std::move(reading), .assessment = std::move(assessment)});
  }
  return view;
}

// The disk health of the MyClean service (it runs as SYSTEM and reads every disk's SMART data
// or NVMe log); without it, what this process reads itself.
health::HealthView disk_health(const fs::path& data_dir, WallTime now,
                               std::vector<std::string>& notes) {
  const mcp::Reply reply = pipe_caller(std::wstring(platform::default_pipe_name), service_timeout)(
      ipc::disks_health_method, nlohmann::json::object());
  const Result<nlohmann::json> service =
      reply ? Result<nlohmann::json>(*reply) : fail(Errc::io_error, reply.error().message);
  std::vector<std::string> local_notes;
  scanall::ChosenHealth chosen =
      scanall::choose_health(service, [&] { return read_health(data_dir, now, local_notes); });
  notes.push_back(std::move(chosen.note));
  std::ranges::move(local_notes, std::back_inserter(notes));
  return std::move(chosen.view);
}

// A fixed volume with a drive letter, and one of `wanted` when that is not empty.
bool selected(const telemetry::VolumeStatus& volume, std::span<const std::wstring> wanted) {
  return std::ranges::any_of(volume.mount_points, [wanted](const std::string& mount) {
    const std::wstring root = placement::drive_root(utf8_path(mount));
    return !root.empty() && root.size() == mount.size() &&
           ::GetDriveTypeW(root.c_str()) == DRIVE_FIXED &&
           (wanted.empty() || std::ranges::find(wanted, root) != wanted.end());
  });
}

// True when `root` is one of `wanted`, or `wanted` is empty.
bool wanted_root(const fs::path& root, std::span<const std::wstring> wanted) {
  return wanted.empty() || std::ranges::find(wanted, placement::drive_root(root)) != wanted.end();
}

// Everything the per-drive analyses use. Every pointer outlives the run.
struct Run {
  const Request* request = nullptr;
  const Knowledge* knowledge = nullptr;
  const clean::IEnvironment* environment = nullptr;
  const clean::IFileTree* files = nullptr;
  const os::IClock* clock = nullptr;
  std::vector<fs::path> in_use;  // folders of running processes
  WallTime now;
  std::vector<DriveInfo> drives;   // the volumes scanned
  std::vector<DriveInfo> targets;  // every fixed volume: where findings may move data to
  std::vector<Finding> findings;
  std::vector<space::ScanResult> trees;
  std::ostream* err = nullptr;
};

void append(std::vector<Finding>& findings, std::vector<Finding> more) {
  std::ranges::move(more, std::back_inserter(findings));
}

Result<space::ScanResult> index_tree(DriveInfo& drive) {
  Result<std::unique_ptr<platform::Win32NtfsVolume>> volume =
      platform::Win32NtfsVolume::open(drive.root);
  if (!volume) {
    return tl::unexpected(volume.error());
  }
  Result<index::IndexedVolume> indexed = index::IndexedVolume::build(**volume);
  if (!indexed) {
    return tl::unexpected(indexed.error());
  }
  drive.tree = "index";
  drive.complete = true;
  return indexed->index().to_scan_result(drive.root);
}

// The volume's tree: from its $MFT when elevated, else scanner v1 stopped after the budget.
Result<space::ScanResult> build_tree(Run& run, DriveInfo& drive) {
  std::ostream& err = *run.err;
  if (platform::process_elevated()) {
    Result<space::ScanResult> tree = index_tree(drive);
    if (tree) {
      return tree;
    }
    drive.notes.push_back("index: " + tree.error().message + "; scanner v1 used");
  }
  drive.tree = "scanner";
  const Deadline deadline(run.request->budget);
  const platform::Win32DirectoryReader reader;
  Result<space::ScanResult> tree =
      space::scan(reader, drive.root, {.workers = 4, .progress_every = 100'000}, deadline.token(),
                  [&err](const space::ScanProgress& progress) {
                    err << std::format("\r  tree: {} folders, {} entries, {}", progress.folders,
                                       progress.files, scanall::size_text(progress.size_on_disk))
                        << std::flush;
                  });
  err << '\n';
  if (tree) {
    drive.complete = !tree->cancelled;
    if (tree->cancelled) {
      drive.notes.push_back(std::format(
          "the tree stopped after {} min: findings cover what it listed; cold data not judged",
          run.request->budget.count()));
    }
  }
  return tree;
}

std::optional<map::DiskMap> map_drive(const Run& run, const space::ScanResult& tree,
                                      DriveInfo& drive) {
  Result<map::DiskMap> map =
      map::build_map(tree, run.knowledge->areas, *run.environment,
                     map::VolumeSpace{.total = drive.size, .free = drive.free},
                     {.min_size = min_compress_folder, .depth = 0});
  if (!map) {
    drive.notes.push_back("map: " + map.error().message);
    return std::nullopt;
  }
  drive.scanned = map->scanned;
  drive.explained = map->explained;
  drive.not_visible = map->not_visible;
  return std::move(*map);
}

// The folder of `tree` at `path`, by name from the root (case-insensitive); nullopt when the
// tree does not hold it.
std::optional<std::size_t> find_folder(const space::ScanResult& tree, const fs::path& path) {
  if (placement::drive_root(path) != placement::drive_root(tree.root().path)) {
    return std::nullopt;
  }
  std::size_t current = 0;
  for (const fs::path& part : path.relative_path()) {
    if (part.empty()) {
      continue;
    }
    const std::vector<std::size_t>& children = tree.folders[current].folders;
    const auto found = std::ranges::find_if(children, [&](std::size_t child) {
      return placement::same_path(tree.folders[child].path.filename(), part);
    });
    if (found == children.end()) {
      return std::nullopt;
    }
    current = *found;
  }
  return current;
}

bool compressible_category(map::Category category) {
  return category == map::Category::apps || category == map::Category::games ||
         category == map::Category::dev_toolchains;
}

// The largest folders of compressible categories (programs, games, toolchains, and the packs'
// compression targets), at most max_compress_folders.
std::vector<std::size_t> compress_folders(const Run& run, const space::ScanResult& tree,
                                          const map::DiskMap* map) {
  const fs::path windows = platform::windows_folder();
  std::vector<std::size_t> folders;
  const auto add = [&](const fs::path& path) {
    const std::optional<std::size_t> index = find_folder(tree, path);
    if (index && tree.folders[*index].size_on_disk >= min_compress_folder &&
        !compress::is_within(path, windows) &&
        std::ranges::find(folders, *index) == folders.end()) {
      folders.push_back(*index);
    }
  };
  if (map != nullptr) {
    for (const map::Match& match : map->matches) {
      if (!match.file && compressible_category(map->areas[match.area].category)) {
        add(match.path);
      }
    }
  }
  for (const packs::CompressTarget& target : run.knowledge->compress) {
    for (const std::string& pattern : target.paths) {
      const std::optional<std::wstring> expanded =
          clean::expand_variables(utf8_path(pattern).native(), *run.environment);
      if (!expanded) {
        continue;
      }
      for (const clean::Entry& entry : clean::resolve(*run.files, fs::path{*expanded}).entries) {
        if (entry.stat.kind == os::EntryKind::directory) {
          add(entry.path);
        }
      }
    }
  }
  std::ranges::sort(folders, [&tree](std::size_t left, std::size_t right) {
    return tree.folders[left].size_on_disk > tree.folders[right].size_on_disk;
  });
  if (folders.size() > max_compress_folders) {
    folders.resize(max_compress_folders);
  }
  return folders;
}

void estimate_compression(Run& run, const space::ScanResult& tree, const map::DiskMap* map,
                          DriveInfo& drive) {
  if (!scanall::healthy(drive)) {
    drive.notes.emplace_back("compression not estimated: the drive's health is not good");
    return;
  }
  const platform::Win32CompressionProbe probe;
  const compress::AnalyzerOptions options{
      .rules = {.cluster = platform::cluster_size(drive.root).value_or(Bytes{4096}),
                .windows = platform::windows_folder()},
      .budget = Bytes{std::uint64_t{32} << 20U},
      .time_budget = std::chrono::seconds{30}};
  for (const std::size_t folder : compress_folders(run, tree, map)) {
    *run.err << "  compression: " << space::utf8(tree.folders[folder].path) << '\n';
    const compress::FolderEstimate estimate =
        compress::estimate(tree, folder, probe, *run.clock, options);
    if (std::optional<Finding> finding = scanall::compress_finding(estimate, drive)) {
      run.findings.push_back(*std::move(finding));
    }
  }
}

void analyze_drive(Run& run, std::size_t index, const scanall::DuplicateOptions& duplicates) {
  DriveInfo& drive = run.drives[index];
  *run.err << std::format("[{}/{}] {}\n", index + 1, run.drives.size(), space::utf8(drive.root));
  Result<space::ScanResult> tree = build_tree(run, drive);
  if (!tree) {
    drive.notes.push_back("no tree: " + tree.error().message);
    return;
  }
  const std::optional<map::DiskMap> map = map_drive(run, *tree, drive);
  if (map) {
    append(run.findings,
           scanall::map_findings(*map, drive, run.knowledge->rules, run.targets, min_finding));
  }
  const scanall::FolderTimes times = scanall::folder_times(*tree);
  if (drive.complete) {
    append(run.findings, scanall::find_cold(*tree, times, drive, run.targets, run.in_use, run.now));
  }
  append(run.findings, scanall::advise_placement(*tree, times, map ? &*map : nullptr, drive,
                                                 run.targets, run.now));
  estimate_compression(run, *tree, map ? &*map : nullptr, drive);
  // Only files that can be duplicates stay in memory for the search across drives.
  for (space::Folder& folder : tree->folders) {
    std::erase_if(folder.files, [&duplicates](const space::FileItem& file) {
      return file.size < duplicates.min_size;
    });
  }
  run.trees.push_back(*std::move(tree));
}

bool on_selected_drive(const Run& run, const fs::path& path) {
  return scanall::drive_of(run.drives, path) != nullptr;
}

void evaluate_rules(Run& run) {
  Result<safety::ProtectedLocations> locations = platform::protected_locations(nullptr);
  if (!locations) {
    run.drives.front().notes.push_back("cleaning rules not evaluated: " +
                                       locations.error().message);
    return;
  }
  const platform::Win32Usage usage;
  const clean::RuleEngine engine(*run.files, *run.environment, usage, *std::move(locations));
  *run.err << "cleaning rules\n";
  append(run.findings, scanall::rule_findings(engine.evaluate(run.knowledge->rules, run.now),
                                              "rules", run.findings, run.drives));
  if (!run.knowledge->workspaces) {
    return;
  }
  clean::Workspaces workspaces = *run.knowledge->workspaces;
  std::erase_if(workspaces.roots,
                [&run](const fs::path& root) { return !on_selected_drive(run, root); });
  const std::optional<fs::path> git = platform::find_git();
  if (workspaces.roots.empty() || !git) {
    return;
  }
  *run.err << "developer artifacts\n";
  const platform::GitCli cli(*git);
  append(run.findings, scanall::rule_findings(
                           engine.scan_artifacts(run.knowledge->rules, workspaces, cli, run.now),
                           "artifacts", run.findings, run.drives));
}

DriveInfo* find_drive(std::vector<DriveInfo>& drives, const fs::path& root) {
  const auto found = std::ranges::find_if(drives, [&root](const DriveInfo& drive) {
    return placement::drive_root(drive.root) == placement::drive_root(root);
  });
  return found == drives.end() ? nullptr : &*found;
}

void analyze_maintenance(Run& run, std::span<const drives::DriveState> states) {
  *run.err << "drive maintenance\n";
  const platform::Win32DirectoryReader reader;
  const platform::Win32VolumeLayout layout;
  const platform::Win32OptimizerLog log;
  for (const drives::VolumeAnalysis& analysis : drives::analyze(
           states, {.reader = &reader, .layout = &layout, .log = &log, .clock = run.clock}, {})) {
    if (std::optional<Finding> finding = scanall::maintenance_finding(analysis)) {
      run.findings.push_back(*std::move(finding));
    }
    if (DriveInfo* drive = find_drive(run.drives, analysis.drive.root); drive != nullptr) {
      for (const std::string* note : {&analysis.history_note, &analysis.free_space_note}) {
        if (!note->empty()) {
          drive->notes.push_back("maintenance: " + *note);
        }
      }
    }
  }
}

scanall::DuplicateOptions duplicate_options(const Run& run) {
  scanall::DuplicateOptions options;
  if (run.knowledge->workspaces) {
    options.preferred = run.knowledge->workspaces->roots;
  }
  if (const std::optional<std::wstring> profile = run.environment->variable(L"USERPROFILE")) {
    for (const wchar_t* library : {L"Documents", L"Pictures", L"Music", L"Videos", L"Desktop"}) {
      options.preferred.push_back(fs::path{*profile} / library);
    }
    options.transient.push_back(fs::path{*profile} / L"Downloads");
  }
  if (const std::optional<std::wstring> temp = run.environment->variable(L"TEMP")) {
    options.transient.emplace_back(*temp);
  }
  for (const DriveInfo& drive : run.drives) {
    options.transient.push_back(drive.root / L"$Recycle.Bin");
  }
  return options;
}

void find_duplicates(Run& run, const scanall::DuplicateOptions& options,
                     std::vector<std::string>& notes) {
  *run.err << "duplicates across drives\n";
  const platform::Win32ContentHasher hasher;
  const Deadline deadline(run.request->budget);
  scanall::DuplicateRun found =
      scanall::find_duplicates(run.trees, run.drives, hasher, options, deadline.token());
  notes.push_back(std::format(
      "duplicates: {} sets of identical files of at least {}, {} hashed of a {} budget{}{}, {} "
      "files unreadable",
      found.groups, scanall::size_text(options.min_size), scanall::size_text(found.hashed),
      scanall::size_text(options.budget),
      found.skipped > 0 ? std::format("; {} hashes did not fit the budget", found.skipped) : "",
      deadline.expired() ? "; stopped by the time budget" : "", found.errors));
  append(run.findings, std::move(found.findings));
}

std::string plan_text(const scanall::Plan& plan) {
  std::string text(scanall::to_string(plan.kind));
  for (const auto& [name, value] : plan.params) {
    text += std::format(" {}={}", name, value);
  }
  return text;
}

std::string drive_text(const scanall::DriveSummary& summary) {
  const DriveInfo& drive = summary.drive;
  const std::uint64_t used = drive.size.count() - std::min(drive.size.count(), drive.free.count());
  std::string text = std::format(
      "  {}  {:<8} health {:<8} size {:>10}  used {:>10}  free {:>10}  explained {}  tree {}{}\n",
      space::utf8(drive.root), placement::to_string(drive.drive_class),
      health::to_string(drive.health), scanall::size_text(drive.size),
      scanall::size_text(Bytes{used}), scanall::size_text(drive.free),
      used == 0 ? std::string("-")
                : std::format("{:.1f} %", static_cast<double>(drive.explained.count()) * 100.0 /
                                              static_cast<double>(used)),
      drive.tree.empty() ? "none" : drive.tree, drive.complete ? "" : " (partial)");
  if (!drive.health_note.empty()) {
    text += "      health: " + drive.health_note + "\n";
  }
  if (drive.not_visible) {
    text += std::format("      not visible to this scan: {}\n",
                        scanall::size_text(Bytes{static_cast<std::uint64_t>(
                            std::max<std::int64_t>(*drive.not_visible, 0))}));
  }
  std::string counted;
  for (std::size_t group = 0; group < scanall::action_group_count; ++group) {
    if (summary.counted.at(group).count() > 0) {
      counted += std::format("{}{} {}", counted.empty() ? "" : ", ",
                             scanall::to_string(static_cast<scanall::ActionGroup>(group)),
                             scanall::size_text(summary.counted.at(group)));
    }
  }
  text += "      counted: " + (counted.empty() ? std::string("nothing") : counted) + "\n";
  for (const std::string& note : drive.notes) {
    text += "      note: " + note + "\n";
  }
  return text;
}

std::string finding_text(const Finding& finding) {
  return std::format(
      "  {:>10} of {:>10}  {}  [{} {:.2f}{}{}]\n      {}\n      plan: {}\n",
      scanall::size_text(finding.counted), scanall::size_text(finding.bytes),
      finding.path.empty() ? space::utf8(finding.drive) : space::utf8(finding.path), finding.source,
      finding.confidence, finding.urgent ? " urgent" : "",
      finding.items.size() > 1 ? std::format(" {} items", finding.items.size()) : std::string(),
      finding.reason, plan_text(finding.plan));
}

std::string render(const scanall::Report& report) {
  std::string text = std::format(
      "MyClean scan-all {:%Y-%m-%d %H:%M} UTC. Read-only: nothing was changed.\n"
      "Each byte is counted once, under the first action that covers it: remove > deduplicate > "
      "relocate > archive > compress > defrag-trim > uninstall-review > manual-review.\n\nDrives\n",
      std::chrono::floor<std::chrono::minutes>(report.time));
  for (const scanall::DriveSummary& drive : report.drives) {
    text += drive_text(drive);
  }
  text += "\nTotals (counted bytes)\n";
  for (std::size_t group = 0; group < scanall::action_group_count; ++group) {
    const auto action = static_cast<scanall::ActionGroup>(group);
    text += std::format("  {:<17} {:>10}  {} findings\n", scanall::to_string(action),
                        scanall::size_text(report.totals.at(group)),
                        std::ranges::count(report.findings, action, &Finding::group));
  }
  std::optional<scanall::ActionGroup> current;
  for (const Finding& finding : report.findings) {
    if (finding.group != current) {
      current = finding.group;
      text += std::format("\n== {}\n", scanall::to_string(finding.group));
    }
    text += finding_text(finding);
  }
  text += "\nNotes\n";
  for (const std::string& note : report.notes) {
    text += "  " + note + "\n";
  }
  return text;
}

}  // namespace

int scan_all_command(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
  const Result<Request> request = parse(args);
  if (!request) {
    err << "myclean: scan-all: " << request.error().message << '\n' << usage_text;
    return exit_usage;
  }
  const ProcessBackground priority;
  const os::SystemClock clock;
  const platform::Win32Environment environment;
  const platform::Win32FileTree files;
  const fs::path data_dir = request->data_dir.value_or(default_data_dir());
  std::vector<std::string> notes;
  if (!platform::process_elevated()) {
    notes.emplace_back(
        "not elevated: trees come from scanner v1, which cannot see folders this "
        "account may not list nor the NTFS metadata files");
  }
  const Knowledge knowledge = load_knowledge(data_dir, notes);

  const platform::Win32VolumeInfo volume_info;
  Result<std::vector<telemetry::VolumeStatus>> volumes = volume_info.volumes();
  if (!volumes) {
    err << "myclean: scan-all: " << volumes.error().message << '\n';
    return exit_unavailable;
  }
  // Every fixed volume: the ones not scanned are still targets of archive and relocate findings.
  std::erase_if(*volumes,
                [](const telemetry::VolumeStatus& volume) { return !selected(volume, {}); });
  err << "disk health\n";
  Run run{.request = &*request,
          .knowledge = &knowledge,
          .environment = &environment,
          .files = &files,
          .clock = &clock,
          .in_use = platform::Win32Usage{}.process_folders(),
          .now = clock.now(),
          .err = &err};
  const health::HealthView view = disk_health(data_dir, run.now, notes);
  const platform::Win32SeekPenalty seek;
  const platform::Win32TrimSupport trim;
  std::vector<drives::DriveState> states = drives::drive_states(*volumes, &view, seek, trim);
  run.targets = scanall::drive_infos(
      states, view, fs::path{environment.variable(L"SystemDrive").value_or(L"C:") + L"\\"});
  std::erase_if(states, [&request](const drives::DriveState& state) {
    return !wanted_root(state.root, request->drives);
  });
  std::ranges::copy_if(
      run.targets, std::back_inserter(run.drives),
      [&request](const DriveInfo& drive) { return wanted_root(drive.root, request->drives); });
  if (run.drives.empty()) {
    err << "myclean: scan-all: no fixed volume with a drive letter matches\n";
    return exit_usage;
  }

  const scanall::DuplicateOptions duplicates = duplicate_options(run);
  for (std::size_t index = 0; index < run.drives.size(); ++index) {
    analyze_drive(run, index, duplicates);
  }
  evaluate_rules(run);
  analyze_maintenance(run, states);
  find_duplicates(run, duplicates, notes);

  scanall::Report report =
      scanall::build_report(std::move(run.drives), std::move(run.findings), run.trees, run.now);
  report.notes = std::move(notes);
  out << (request->json ? scanall::to_json(report).dump(-1, ' ', false,
                                                        nlohmann::json::error_handler_t::replace) +
                              "\n"
                        : render(report));
  return exit_ok;
}

}  // namespace myclean::cli
