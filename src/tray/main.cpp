// myclean-tray.exe: the tray icon and the dashboard, in the owner's session (deploy contract C5).

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/service/system.hpp"
#include "tray/app.hpp"
#include "tray/instance.hpp"

namespace {

constexpr std::size_t log_file_size = std::size_t{1} << 20U;
constexpr std::size_t log_files = 3;

// %LOCALAPPDATA%\MyClean\logs\myclean-tray.log, rotating; without it the tray runs unlogged.
// spdlog opens files by narrow name, read in the ANSI code page, as in the service.
void open_log() {
  const myclean::Result<std::filesystem::path> local = myclean::platform::local_app_data_dir();
  if (!local) {
    return;
  }
  const std::filesystem::path folder = *local / "MyClean" / "logs";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  const std::u8string file = (folder / "myclean-tray.log").u8string();
  try {
    auto logger = std::make_shared<spdlog::logger>(
        "myclean-tray", std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                            std::string(file.begin(), file.end()), log_file_size, log_files));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
  } catch (const spdlog::spdlog_ex&) {
    // The folder is unwritable: run unlogged (a Windows app has no console to log to).
    spdlog::set_level(spdlog::level::off);
  }
}

}  // namespace

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): winbase.h's names
int WINAPI wWinMain(HINSTANCE module, HINSTANCE /*previous*/, PWSTR /*command_line*/,
                    int /*show*/) {
  using namespace myclean;
  open_log();
  const Result<std::optional<tray::Instance>> instance =
      tray::Instance::acquire(tray::instance_mutex_name, tray::quit_event_name);
  if (!instance) {
    spdlog::error("single-instance check: {}", instance.error().message);
    return tray::exit_start_failed;
  }
  if (!*instance) {
    spdlog::info("another tray runs in this session; exiting");
    return tray::exit_ok;
  }
  spdlog::info("tray started");
  const int code = tray::run(module, **instance, platform::default_pipe_name);
  spdlog::info("tray stopped (exit code {})", code);
  return code;
}
