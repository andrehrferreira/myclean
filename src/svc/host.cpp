#include "svc/host.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <windows.h>

#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "platform/ipc/security.hpp"
#include "platform/service/crash_dump.hpp"
#include "platform/service/system.hpp"
#include "svc/component.hpp"
#include "svc/service.hpp"

namespace myclean::svc {

namespace {

namespace fs = std::filesystem;

constexpr DWORD tick_ms = 1000;
constexpr std::size_t log_file_size = std::size_t{5} << 20U;  // 5 MiB
constexpr std::size_t log_files = 3;

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// The ASCII text of `text`, or nothing when it has another character.
std::optional<std::string> ascii(std::wstring_view text) {
  std::string result;
  for (const wchar_t c : text) {
    if (c >= 0x80) {
      return std::nullopt;
    }
    result.push_back(static_cast<char>(c));
  }
  return result;
}

std::string_view mode_name(Mode mode) {
  return mode == Mode::service ? "service" : "console";
}

Result<fs::path> state_dir_of(const HostOptions& options) {
  if (options.mode == Mode::service) {
    return platform::program_data_dir().transform(
        [](const fs::path& dir) { return dir / "MyClean"; });
  }
  if (options.state_dir) {
    return *options.state_dir;
  }
  return platform::local_app_data_dir().transform(
      [](const fs::path& dir) { return dir / "MyClean" / "console"; });
}

// spdlog opens files by narrow name, which Windows reads in the ANSI code page; a log folder
// whose path has characters outside it cannot be opened.
Result<void> open_log(const fs::path& dir, bool console) {
  try {
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        utf8(dir / "myclean-svc.log"), log_file_size, log_files)};
    if (console) {
      sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }
    auto logger = std::make_shared<spdlog::logger>("myclean-svc", sinks.begin(), sinks.end());
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
    return {};
  } catch (const spdlog::spdlog_ex& error) {
    return fail(Errc::io_error, "opening the log in " + utf8(dir) + ": " + error.what());
  }
}

// The Service options of this run; in service mode also records the owner, secures the state
// folder and installs crash dumps.
Result<ServiceOptions> prepare(const HostOptions& options, const fs::path& state_dir,
                               const fs::path& exe, const InstallLayout& layout) {
  ServiceOptions service{
      .database = state_dir / "myclean.db",
      .data_dir = layout.data_dir,
      .pipe_name = options.pipe_name,
      .owner_sid = {},
      .mode = std::string(mode_name(options.mode)),
      .version = manifest_version(layout.manifest),
      .tray_image = options.tray_image.value_or(exe.parent_path() / "myclean-tray.exe")};
  if (options.mode == Mode::console) {
    Result<std::string> owner = platform::current_user_sid();
    if (!owner) {
      return tl::unexpected(owner.error());
    }
    service.owner_sid = std::move(*owner);
    return service;
  }
  constexpr std::array<std::string_view, 2> trusted{platform::system_sid,
                                                    platform::administrators_sid};
  Result<std::string> owner = resolve_owner(state_dir, trusted);
  if (!owner) {
    return tl::unexpected(owner.error());
  }
  if (Result<void> secured = platform::protect_state_dir(state_dir, *owner); !secured) {
    return tl::unexpected(secured.error());
  }
  if (Result<void> dumps = platform::install_crash_dumps(state_dir / "dumps"); !dumps) {
    return tl::unexpected(dumps.error());
  }
  service.owner_sid = std::move(*owner);
  return service;
}

}  // namespace

Result<HostOptions> parse_command_line(std::span<const std::wstring> args) {
  HostOptions options;
  if (args.empty()) {
    return options;
  }
  if (args.front() != L"--console") {
    return fail(Errc::invalid_argument, "unknown argument");
  }
  options.mode = Mode::console;
  for (std::size_t index = 1; index < args.size(); index += 2) {
    const std::wstring& name = args[index];
    if (index + 1 >= args.size() ||
        (name != L"--state" && name != L"--pipe" && name != L"--tray" && name != L"--sandbox")) {
      return fail(Errc::invalid_argument, "unknown or incomplete argument");
    }
    const std::wstring& value = args[index + 1];
    if (name == L"--state" || name == L"--tray" || name == L"--sandbox") {
      std::error_code error;
      const fs::path absolute = fs::absolute(value, error).lexically_normal();
      if (error) {
        return fail(Errc::invalid_argument, "--state, --tray, --sandbox: " + error.message());
      }
      if (name == L"--state") {
        options.state_dir = absolute;
      } else if (name == L"--tray") {
        options.tray_image = absolute;
      } else {
        options.sandbox = absolute;
      }
    } else {
      const std::optional<std::string> pipe = ascii(value);
      if (!pipe || !ipc::is_token(*pipe)) {
        return fail(Errc::invalid_argument, "--pipe: the name must be a token");
      }
      options.pipe_name = LR"(\\.\pipe\)" + value;
    }
  }
  return options;
}

std::string_view usage() {
  return "usage: myclean-svc.exe                      run as the MyClean service (SCM only)\n"
         "       myclean-svc.exe --console [--state <dir>] [--pipe <name>] [--tray <exe>]\n"
         "                                 [--sandbox <dir>]\n"
         "                                            run in this console until Ctrl+C; cleaning\n"
         "                                            runs only on the rules of <dir>\\data\n";
}

InstallLayout install_layout(const fs::path& exe) {
  const fs::path version_dir = exe.parent_path().parent_path();
  return InstallLayout{.data_dir = version_dir / "data", .manifest = version_dir / "manifest.json"};
}

std::optional<std::string> manifest_version(const fs::path& manifest) {
  std::ifstream file(manifest, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
  if (!value.is_object()) {
    return std::nullopt;
  }
  const auto version = value.find("version");
  if (version == value.end() || !version->is_string()) {
    return std::nullopt;
  }
  return version->get<std::string>();
}

Result<std::string> resolve_owner(const fs::path& state_dir,
                                  std::span<const std::string_view> trusted_owners) {
  const fs::path file = state_dir / "owner.sid";
  std::error_code error;
  if (fs::exists(file, error)) {
    const Result<std::string> file_owner = platform::file_owner_sid(file);
    if (!file_owner) {
      return tl::unexpected(file_owner.error());
    }
    if (std::ranges::find(trusted_owners, *file_owner) == trusted_owners.end()) {
      return fail(Errc::refused, utf8(file) + " belongs to " + *file_owner +
                                     ", not to SYSTEM or Administrators; an administrator must "
                                     "delete it so the console user is recorded again");
    }
    std::ifstream in(file, std::ios::binary);
    std::string sid;
    std::getline(in, sid);
    while (!sid.empty() && (sid.back() == '\r' || sid.back() == ' ')) {
      sid.pop_back();
    }
    if (!platform::check_sid(sid)) {
      return fail(Errc::invalid_data, utf8(file) + " does not hold a SID");
    }
    return sid;
  }
  Result<std::string> console = platform::console_user_sid();
  if (!console) {
    return fail(console.error().code,
                "no owner recorded in " + utf8(file) +
                    " and the console user is unknown: " + console.error().message);
  }
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << *console << '\n';
  out.close();
  if (!out) {
    return fail(Errc::io_error, "cannot write " + utf8(file));
  }
  return console;
}

int run(const HostOptions& options, std::vector<std::unique_ptr<IComponent>> components,
        HANDLE stop_event, const std::function<void()>& report_running) {
  const Result<fs::path> state_dir = state_dir_of(options);
  const Result<fs::path> exe = platform::executable_path();
  if (!state_dir || !exe) {
    std::cerr << "myclean-svc: " << (state_dir ? exe.error() : state_dir.error()).message << '\n';
    return exit_start_failed;
  }
  std::error_code error;
  fs::create_directories(*state_dir / "logs", error);
  if (error) {
    std::cerr << "myclean-svc: creating " << utf8(*state_dir / "logs") << ": " << error.message()
              << '\n';
    return exit_start_failed;
  }
  if (Result<void> log = open_log(*state_dir / "logs", options.mode == Mode::console); !log) {
    std::cerr << "myclean-svc: " << log.error().message << '\n';
    return exit_start_failed;
  }

  const InstallLayout layout = install_layout(*exe);
  spdlog::info("myclean-svc {} starting in {} mode; executable {}, data {}{}, state {}",
               MYCLEAN_VERSION, mode_name(options.mode), utf8(*exe), utf8(layout.data_dir),
               fs::is_directory(layout.data_dir, error) ? "" : " (missing)", utf8(*state_dir));

  const os::SystemClock clock;
  Result<std::unique_ptr<Service>> service =
      prepare(options, *state_dir, *exe, layout).and_then([&](ServiceOptions service_options) {
        return Service::start(std::move(service_options), std::move(components), clock);
      });
  if (!service) {
    spdlog::critical("start failed ({}): {}", to_string(service.error().code),
                     service.error().message);
    return exit_start_failed;
  }
  report_running();
  spdlog::info("running");

  while (::WaitForSingleObject(stop_event, tick_ms) == WAIT_TIMEOUT) {
    (*service)->tick();
  }
  spdlog::info("stopping");
  const Result<void> stopped = (*service)->stop();
  spdlog::info("stopped");
  return stopped ? exit_ok : exit_stop_failed;
}

}  // namespace myclean::svc
