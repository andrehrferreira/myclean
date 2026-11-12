#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <d3d11.h>
#include <dxgi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/tray_model/poller.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/poll_thread.hpp"

struct ImGuiContext;
struct ImPlotContext;

namespace myclean::tray {

// The dashboard: a top-level window drawn with Dear ImGui and ImPlot on Direct3D 11. It renders
// only when something changed (input, new data, resize), a few frames each time so that ImGui
// settles, and nothing while minimized. Closing it destroys everything, so a closed dashboard
// costs no memory. UI thread only.
class DashboardWindow {
 public:
  using Send = std::function<void(tray_model::Request)>;

  // Creates and shows the window. `shared` and `zone` must outlive it. `send` takes the owner's
  // requests (snooze, dismiss, explain, chart range) to the poll thread. Called on the owner's
  // click: the tray log gets the time from here to the first frame presented (NFR-PERF-2).
  [[nodiscard]] static Result<std::unique_ptr<DashboardWindow>> open(
      HINSTANCE instance, SharedModel& shared, const std::chrono::time_zone& zone, Send send);

  DashboardWindow(const DashboardWindow&) = delete;
  DashboardWindow& operator=(const DashboardWindow&) = delete;
  DashboardWindow(DashboardWindow&&) = delete;
  DashboardWindow& operator=(DashboardWindow&&) = delete;
  ~DashboardWindow();

  // The owner closed the window; destroy this object.
  [[nodiscard]] bool closed() const { return closed_; }
  [[nodiscard]] bool wants_frame() const { return frames_ > 0 && !minimized_; }
  void request_frames() { frames_ = settle_frames; }
  void render();
  // Brings the window to the front (the owner asked for it again).
  void activate() const;
  // Shows `section` (the owner clicked a toast about it).
  void show(Section section) {
    ui_.section = section;
    request_frames();
  }

 private:
  static constexpr int settle_frames = 3;

  DashboardWindow(SharedModel& shared, const std::chrono::time_zone& zone, Send send)
      : shared_(&shared), zone_(&zone), send_(std::move(send)) {}

  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
  [[nodiscard]] Result<void> create_device();
  [[nodiscard]] Result<void> create_target();
  void set_scale(float scale);

  SharedModel* shared_;
  const std::chrono::time_zone* zone_;
  Send send_;
  UiState ui_;
  wil::unique_hwnd window_;
  wil::com_ptr_nothrow<ID3D11Device> device_;
  wil::com_ptr_nothrow<ID3D11DeviceContext> context_;
  wil::com_ptr_nothrow<IDXGISwapChain> swap_chain_;
  wil::com_ptr_nothrow<ID3D11RenderTargetView> target_;
  ImGuiContext* imgui_ = nullptr;
  ImPlotContext* implot_ = nullptr;
  bool win32_backend_ = false;
  bool dx11_backend_ = false;
  bool closed_ = false;
  bool minimized_ = false;
  bool resized_ = false;
  int frames_ = settle_frames;
  // When open() began; reset once the first frame is presented and its time logged.
  std::optional<std::chrono::steady_clock::time_point> opening_;
};

}  // namespace myclean::tray
