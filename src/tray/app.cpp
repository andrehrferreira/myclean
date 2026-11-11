#include "tray/app.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dwmapi.h>
#include <spdlog/spdlog.h>
#include <wil/resource.h>
#include <windows.h>
#include <windowsx.h>

#include "core/games/fullscreen.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/tray_model/governor.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/tray_model/tray_state.hpp"
#include "core/user_tasks/task.hpp"
#include "platform/games/win32_games.hpp"
#include "platform/input/input_watchdog.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/dashboard_window.hpp"
#include "tray/icon.hpp"
#include "tray/instance.hpp"
#include "tray/pipe_link.hpp"
#include "tray/poll_thread.hpp"
#include "tray/task_worker.hpp"
#include "tray/tray_icon.hpp"
#include "tray/user_tools.hpp"
#include "tray/window_object.hpp"

namespace myclean::tray {

namespace {

constexpr const wchar_t* class_name = L"MyClean.Tray";
constexpr UINT update_message = WM_APP + 2;
constexpr UINT command_open = 1;
constexpr UINT command_quit = 2;
// Each pipe call; also bounds how long quitting waits for the poll thread.
constexpr std::chrono::milliseconds call_timeout{2000};

// The owner's session as governor.user_state takes it: SHQueryUserNotificationState and the
// process of the foreground window. Read on the poll thread; neither call shows anything.
std::optional<tray_model::UserStateReading> read_user_state() {
  const Result<games::NotificationState> state = platform::query_user_notification_state();
  if (!state) {
    return std::nullopt;
  }
  DWORD pid = 0;
  if (HWND foreground = ::GetForegroundWindow(); foreground != nullptr) {
    ::GetWindowThreadProcessId(foreground, &pid);
  }
  return tray_model::UserStateReading{.state = std::string(games::to_string(*state)),
                                      .foreground_pid = static_cast<std::uint32_t>(pid)};
}

// The processes that own a visible top-level window of the owner's desktop, for the intruder
// scan's "no visible window": shown, not cloaked (a suspended Store app's frame), owned by no
// other window and no tool window, as the taskbar and Alt+Tab list them. Reads only.
std::optional<std::vector<std::uint32_t>> read_visible_windows() {
  std::vector<std::uint32_t> pids;
  const auto add = [](HWND window, LPARAM context) -> BOOL {
    auto& found = *std::bit_cast<std::vector<std::uint32_t>*>(context);
    DWORD cloaked = 0;
    const auto style = static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE));
    if (::IsWindowVisible(window) == FALSE || ::GetWindow(window, GW_OWNER) != nullptr ||
        (style & WS_EX_TOOLWINDOW) != 0 ||
        (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
         cloaked != 0)) {
      return TRUE;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid != 0 && std::ranges::find(found, pid) == found.end()) {
      found.push_back(pid);
    }
    return TRUE;
  };
  if (::EnumWindows(add, std::bit_cast<LPARAM>(&pids)) == FALSE) {
    return std::nullopt;
  }
  return pids;
}

class App {
 public:
  App(HINSTANCE module, std::wstring_view pipe_name)
      : module_(module), link_(std::wstring(pipe_name), call_timeout) {}
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = delete;
  App& operator=(App&&) = delete;
  // The worker stops first: it wakes the poll thread, which in turn hands it tasks.
  ~App() {
    if (tasks_) {
      tasks_->stop();
    }
  }

  [[nodiscard]] Result<void> start();
  [[nodiscard]] int loop(HANDLE quit_event);

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
  void refresh_icon(bool add);
  void show_toasts();
  void open_dashboard();
  void show_menu(POINT anchor);
  void show_governor_toasts();

  HINSTANCE module_;
  os::SystemClock clock_;
  SharedModel shared_;
  PipeLink link_;
  wil::unique_hwnd window_;
  std::unique_ptr<TrayIcon> icon_;
  bool icon_shown_ = false;
  wil::unique_hicon image_;
  tray_model::TrayStatus image_status_;
  int image_size_ = 0;
  std::string tooltip_;
  UINT taskbar_created_ = 0;
  std::unique_ptr<DashboardWindow> dashboard_;
  bool quit_ = false;
  // The section a click on the latest toast opens: the governor's after a governor toast.
  std::optional<Section> toast_section_;
  // Filled by the watchdog, emptied by the poll thread: declared before both, so it goes last.
  tray_model::IncidentQueue incidents_;
  std::unique_ptr<platform::InputWatchdog> watchdog_;
  // Runs the approved user-scope plans; the poll thread hands it tasks, so it is joined after it.
  std::unique_ptr<TaskWorker> tasks_;
  std::unique_ptr<PollThread> poll_;  // last: joined first, before anything it posts to goes
};

Result<void> App::start() {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &App::window_proc;
  window_class.hInstance = module_;
  window_class.lpszClassName = class_name;
  if (::RegisterClassExW(&window_class) == 0) {
    return fail(Errc::io_error, "RegisterClassEx failed for the tray window");
  }
  // Never shown: it only receives the icon's and the poll thread's messages. A top-level window
  // (not message-only) so that it also receives the "TaskbarCreated" broadcast.
  HWND window = ::CreateWindowExW(0, class_name, L"MyClean", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                  nullptr, module_, this);
  if (window == nullptr) {
    return fail(Errc::io_error, "CreateWindowEx failed for the tray window");
  }
  window_.reset(window);
  taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");
  icon_ = std::make_unique<TrayIcon>(window);
  refresh_icon(true);
  tasks_ = std::make_unique<TaskWorker>(
      [this](const user_tasks::UserTask& task, const std::stop_token& stop) {
        return run_user_task(task, stop, shared_);
      },
      [this] { poll_->wake(); });
  poll_ = std::make_unique<PollThread>(
      link_, clock_, shared_, [window] { ::PostMessageW(window, update_message, 0, 0); },
      &incidents_, tasks_.get(), &read_user_state, &read_visible_windows);
  // The freeze watchdog: raw input and a clipboard listener on a message-only window, no hook.
  // Without it the tray still runs; the Freezes view says why it is missing.
  Result<std::unique_ptr<platform::InputWatchdog>> watchdog =
      platform::InputWatchdog::start(incidents_, clock_);
  if (watchdog) {
    watchdog_ = std::move(*watchdog);
    spdlog::info("freeze watchdog started");
  } else {
    spdlog::error("freeze watchdog: {}", watchdog.error().message);
    const std::scoped_lock lock(shared_.mutex);
    shared_.model.set_watchdog_error(watchdog.error().message);
  }
  return {};
}

int App::loop(HANDLE quit_event) {
  while (!quit_) {
    const DWORD timeout = dashboard_ && dashboard_->wants_frame() ? 0 : INFINITE;
    const DWORD wait =
        ::MsgWaitForMultipleObjectsEx(1, &quit_event, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_OBJECT_0) {
      spdlog::info("quit event signaled");
      break;
    }
    if (wait == WAIT_FAILED) {
      spdlog::error("MsgWaitForMultipleObjectsEx failed ({})", ::GetLastError());
      return exit_start_failed;
    }
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      if (message.message == WM_QUIT) {
        quit_ = true;
      }
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
    if (dashboard_ && dashboard_->closed()) {
      dashboard_.reset();
      poll_->set_dashboard_open(false);
    } else if (dashboard_ && dashboard_->wants_frame()) {
      dashboard_->render();
    }
  }
  if (watchdog_) {
    watchdog_->stop();
    spdlog::info("freeze watchdog stopped");
  }
  return exit_ok;
}

LRESULT CALLBACK App::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* self = window_object<App>(window, message, lparam);
  if (self == nullptr || !self->window_) {
    return ::DefWindowProcW(window, message, wparam, lparam);
  }
  return self->handle(message, wparam, lparam);
}

LRESULT App::handle(UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == update_message) {
    refresh_icon(false);
    show_toasts();
    show_governor_toasts();
    if (dashboard_) {
      dashboard_->request_frames();
    }
    return 0;
  }
  if (message == TrayIcon::callback_message) {
    switch (LOWORD(lparam)) {
      case NIN_SELECT:
      case NIN_KEYSELECT:
        open_dashboard();
        break;
      case NIN_BALLOONUSERCLICK:
        open_dashboard();
        if (dashboard_ && toast_section_) {
          dashboard_->show(*toast_section_);
        }
        break;
      case WM_CONTEXTMENU:
        show_menu(POINT{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)});
        break;
      default:
        break;
    }
    return 0;
  }
  if (message == taskbar_created_ && taskbar_created_ != 0) {
    refresh_icon(true);  // Explorer restarted and lost every icon
    return 0;
  }
  return ::DefWindowProcW(window_.get(), message, wparam, lparam);
}

void App::refresh_icon(bool add) {
  tray_model::TrayStatus status;
  std::string tooltip;
  {
    const std::scoped_lock lock(shared_.mutex);
    status = shared_.model.tray_status();
    tooltip = shared_.model.tooltip();
  }
  const int size = ::GetSystemMetricsForDpi(SM_CXSMICON, ::GetDpiForWindow(window_.get()));
  const bool new_image = !image_ || status != image_status_ || size != image_size_;
  if (!new_image && !add && icon_shown_ && tooltip == tooltip_) {
    return;
  }
  if (new_image) {
    // The application icon with the state as a dot; the state disc alone without it.
    const Result<std::vector<tray_model::Argb>> base = app_icon_pixels(module_, size);
    if (!base) {
      spdlog::warn("tray icon base: {}", base.error().message);
    }
    Result<wil::unique_hicon> image =
        make_icon(tray_model::icon_pixels(
                      status, size, base ? std::span(*base) : std::span<const tray_model::Argb>{}),
                  size);
    if (!image) {
      spdlog::error("tray icon image: {}", image.error().message);
      return;
    }
    image_ = std::move(*image);
    image_status_ = status;
    image_size_ = size;
  }
  tooltip_ = std::move(tooltip);
  // Not shown yet when the taskbar was not ready at logon: retried at every update.
  const Result<void> shown = add || !icon_shown_ ? icon_->add(image_.get(), tooltip_)
                                                 : icon_->update(image_.get(), tooltip_);
  if (shown.has_value() != icon_shown_) {
    if (shown) {
      spdlog::info("tray icon shown");
    } else {
      spdlog::warn("tray icon: {}", shown.error().message);
    }
  }
  icon_shown_ = shown.has_value();
}

void App::show_toasts() {
  std::vector<tray_model::AlertRow> toasts;
  {
    const std::scoped_lock lock(shared_.mutex);
    toasts = shared_.model.take_toasts();
  }
  if (!icon_shown_) {
    return;  // the alerts stay in the badge and the Alerts section
  }
  for (const tray_model::AlertRow& alert : toasts) {
    TrayIcon::Level level = TrayIcon::Level::info;
    if (alert.severity == tray_model::AlertSeverity::warning) {
      level = TrayIcon::Level::warning;
    } else if (alert.severity == tray_model::AlertSeverity::critical) {
      level = TrayIcon::Level::error;
    }
    if (Result<void> shown = icon_->notify(alert.title, alert.detail, level); !shown) {
      spdlog::warn("toast: {}", shown.error().message);
    } else {
      toast_section_.reset();
    }
  }
}

void App::show_governor_toasts() {
  std::vector<tray_model::GovernorToast> toasts;
  {
    const std::scoped_lock lock(shared_.mutex);
    toasts = shared_.model.take_governor_toasts();
  }
  if (!icon_shown_) {
    return;  // they stay in the badge and the Governor section
  }
  for (const tray_model::GovernorToast& toast : toasts) {
    const TrayIcon::Level level = toast.severe ? TrayIcon::Level::warning : TrayIcon::Level::info;
    if (Result<void> shown = icon_->notify(toast.title, toast.detail, level); !shown) {
      spdlog::warn("governor toast: {}", shown.error().message);
    } else {
      toast_section_ = Section::governor;
    }
  }
}

void App::open_dashboard() {
  if (dashboard_) {
    dashboard_->activate();
    return;
  }
  Result<std::unique_ptr<DashboardWindow>> dashboard = DashboardWindow::open(
      module_, shared_, clock_.zone(),
      [this](tray_model::Request request) { poll_->request(std::move(request)); });
  if (!dashboard) {
    spdlog::error("dashboard: {}", dashboard.error().message);
    return;
  }
  dashboard_ = std::move(*dashboard);
  poll_->set_dashboard_open(true);
}

void App::show_menu(POINT anchor) {
  const wil::unique_hmenu menu(::CreatePopupMenu());
  if (!menu) {
    return;
  }
  ::AppendMenuW(menu.get(), MF_STRING, command_open, L"Open dashboard");
  ::AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu.get(), MF_STRING, command_quit, L"Quit");
  ::SetMenuDefaultItem(menu.get(), command_open, FALSE);
  // The menu closes on a click elsewhere only when its owner is the foreground window.
  ::SetForegroundWindow(window_.get());
  const UINT align = ::GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  const auto command = static_cast<UINT>(::TrackPopupMenuEx(
      menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align,
      anchor.x, anchor.y, window_.get(), nullptr));
  ::PostMessageW(window_.get(), WM_NULL, 0, 0);
  if (command == command_open) {
    open_dashboard();
  } else if (command == command_quit) {
    spdlog::info("quit chosen from the menu");
    quit_ = true;
  }
}

}  // namespace

int run(HINSTANCE module, const Instance& instance, std::wstring_view pipe_name) {
  App app(module, pipe_name);
  if (Result<void> started = app.start(); !started) {
    spdlog::error("start: {}", started.error().message);
    return exit_start_failed;
  }
  return app.loop(instance.quit_event());
}

}  // namespace myclean::tray
