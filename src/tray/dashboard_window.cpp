#include "tray/dashboard_window.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/poll_thread.hpp"
#include "tray/window_object.hpp"

// Declared by the backend header only behind #if 0, so that it does not pull in windows.h.
// NOLINTNEXTLINE(readability-identifier-naming): Dear ImGui's name
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                                             WPARAM wparam, LPARAM lparam);

namespace myclean::tray {

namespace {

constexpr const wchar_t* class_name = L"MyClean.Dashboard";
constexpr int base_width = 1280;
constexpr int base_height = 880;
constexpr float base_dpi = 96.0F;
constexpr std::array<float, 4> clear_color = {0x11 / 255.0F, 0x14 / 255.0F, 0x18 / 255.0F, 1.0F};

Result<void> register_class(HINSTANCE instance, WNDPROC procedure) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = procedure;
  window_class.hInstance = instance;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): IDC_ARROW is a Win32 resource macro
  window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = class_name;
  if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return fail(Errc::io_error, "RegisterClassEx failed");
  }
  return {};
}

// Segoe UI, the Windows UI font, when it is installed; ImGui's own font otherwise.
void load_font(ImGuiIO& io) {
  std::array<wchar_t, MAX_PATH> windows{};
  const UINT length = ::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
  if (length == 0 || length >= windows.size()) {
    return;
  }
  const std::filesystem::path font =
      std::filesystem::path(windows.data()) / "Fonts" / "segoeui.ttf";
  std::error_code error;
  if (!std::filesystem::exists(font, error)) {
    return;
  }
  const std::u8string utf8 = font.u8string();
  const std::string path(utf8.begin(), utf8.end());
  io.Fonts->AddFontFromFileTTF(path.c_str());
}

}  // namespace

Result<std::unique_ptr<DashboardWindow>> DashboardWindow::open(HINSTANCE instance,
                                                               SharedModel& shared,
                                                               const std::chrono::time_zone& zone) {
  if (Result<void> registered = register_class(instance, &DashboardWindow::window_proc);
      !registered) {
    return tl::unexpected(registered.error());
  }
  std::unique_ptr<DashboardWindow> self(new DashboardWindow(shared, zone));
  const float system_scale = static_cast<float>(::GetDpiForSystem()) / base_dpi;
  HWND window = ::CreateWindowExW(0, class_name, L"MyClean", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, static_cast<int>(base_width * system_scale),
                                  static_cast<int>(base_height * system_scale), nullptr, nullptr,
                                  instance, self.get());
  if (window == nullptr) {
    return fail(Errc::io_error, "CreateWindowEx failed for the dashboard");
  }
  self->window_.reset(window);
  const BOOL dark = TRUE;
  ::DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

  if (Result<void> device = self->create_device(); !device) {
    return tl::unexpected(device.error());
  }
  self->imgui_ = ImGui::CreateContext();
  self->implot_ = ImPlot::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini next to the executable
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  load_font(io);
  self->win32_backend_ = ImGui_ImplWin32_Init(window);
  self->dx11_backend_ =
      self->win32_backend_ && ImGui_ImplDX11_Init(self->device_.get(), self->context_.get());
  if (!self->dx11_backend_) {
    return fail(Errc::io_error, "the ImGui Win32 or Direct3D 11 backend did not start");
  }
  self->set_scale(static_cast<float>(::GetDpiForWindow(window)) / base_dpi);
  ::ShowWindow(window, SW_SHOWNORMAL);
  self->activate();
  return self;
}

DashboardWindow::~DashboardWindow() {
  if (imgui_ != nullptr) {
    ImGui::SetCurrentContext(imgui_);
    if (dx11_backend_) {
      ImGui_ImplDX11_Shutdown();
    }
    if (win32_backend_) {
      ImGui_ImplWin32_Shutdown();
    }
    win32_backend_ = false;
    dx11_backend_ = false;
    ImPlot::DestroyContext(implot_);
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
  }
  if (window_) {
    ::SetWindowLongPtrW(window_.get(), GWLP_USERDATA, 0);
    window_.reset();
  }
}

Result<void> DashboardWindow::create_device() {
  DXGI_SWAP_CHAIN_DESC description{};
  description.BufferCount = 2;
  description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.OutputWindow = window_.get();
  description.SampleDesc.Count = 1;
  description.Windowed = TRUE;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  constexpr std::array<D3D_FEATURE_LEVEL, 2> levels = {D3D_FEATURE_LEVEL_11_0,
                                                       D3D_FEATURE_LEVEL_10_0};
  HRESULT result = E_FAIL;
  // WARP, the software rasterizer, when no hardware device is available (a remote session).
  for (const D3D_DRIVER_TYPE driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    result = ::D3D11CreateDeviceAndSwapChain(
        nullptr, driver, nullptr, 0, levels.data(), static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION, &description, swap_chain_.put(), device_.put(), nullptr, context_.put());
    if (SUCCEEDED(result)) {
      break;
    }
  }
  if (FAILED(result)) {
    return fail(Errc::io_error, "D3D11CreateDeviceAndSwapChain failed");
  }
  return create_target();
}

Result<void> DashboardWindow::create_target() {
  wil::com_ptr_nothrow<ID3D11Texture2D> back_buffer;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()))) ||
      FAILED(device_->CreateRenderTargetView(back_buffer.get(), nullptr, target_.put()))) {
    return fail(Errc::io_error, "the dashboard's render target could not be created");
  }
  return {};
}

void DashboardWindow::set_scale(float scale) {
  ImGui::SetCurrentContext(imgui_);
  ImPlot::SetCurrentContext(implot_);
  apply_style(scale);
}

void DashboardWindow::render() {
  ImGui::SetCurrentContext(imgui_);
  ImPlot::SetCurrentContext(implot_);
  if (resized_) {
    resized_ = false;
    target_.reset();
    if (FAILED(swap_chain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)) || !create_target()) {
      closed_ = true;  // the device is gone; the owner can open the dashboard again
      return;
    }
  }
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  {
    const std::scoped_lock lock(shared_->mutex);
    draw_dashboard(shared_->model, *zone_, ui_);
  }
  ImGui::Render();
  const std::array<ID3D11RenderTargetView*, 1> targets = {target_.get()};
  context_->OMSetRenderTargets(1, targets.data(), nullptr);
  context_->ClearRenderTargetView(target_.get(), clear_color.data());
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  swap_chain_->Present(1, 0);  // waits for the vertical blank: at most one frame per refresh
  --frames_;
}

void DashboardWindow::activate() const {
  if (::IsIconic(window_.get()) != FALSE) {
    ::ShowWindow(window_.get(), SW_RESTORE);
  }
  ::SetForegroundWindow(window_.get());
}

LRESULT CALLBACK DashboardWindow::window_proc(HWND window, UINT message, WPARAM wparam,
                                              LPARAM lparam) {
  auto* self = window_object<DashboardWindow>(window, message, lparam);
  if (self == nullptr || !self->window_) {
    return ::DefWindowProcW(window, message, wparam, lparam);
  }
  return self->handle(message, wparam, lparam);
}

LRESULT DashboardWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
  HWND window = window_.get();
  request_frames();
  if (win32_backend_) {
    ImGui::SetCurrentContext(imgui_);
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam) != 0) {
      return 1;
    }
  }
  switch (message) {
    case WM_SIZE:
      minimized_ = wparam == SIZE_MINIMIZED;
      resized_ = resized_ || !minimized_;
      return 0;
    case WM_DPICHANGED: {
      // NOLINTNEXTLINE(*-reinterpret-cast,performance-no-int-to-ptr): Win32 API
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      set_scale(static_cast<float>(LOWORD(wparam)) / base_dpi);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0U) == SC_KEYMENU) {
        return 0;  // Alt would open a window menu the dashboard does not have
      }
      break;
    case WM_CLOSE:
      closed_ = true;
      ::ShowWindow(window, SW_HIDE);
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace myclean::tray
