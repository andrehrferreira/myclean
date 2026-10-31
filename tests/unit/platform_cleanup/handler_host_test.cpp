#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>
// clang-format off
#include <emptyvc.h>  // after windows.h
#include <wrl/client.h>
#include <wrl/implements.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/cleanup/win32_handler_host.hpp"

namespace myclean::platform {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr DWORD pace_ms = 10;
constexpr int window_size = 200;

enum class Scan {
  report,          // reports `space`
  wait_for_abort,  // reports progress until the host answers E_ABORT
  block,           // ignores the host until `release` is set
  show_window,     // opens a visible window, then waits for the abort
};

// What the fake handler does and what was asked of it. Shared with the worker thread.
struct Script {
  HRESULT initialize = S_OK;
  DWORDLONG space = 0;
  Scan scan = Scan::report;
  std::atomic<DWORD> flags_in{0xFFFFFFFF};
  std::atomic<int> scans{0};
  std::atomic<int> purges{0};
  std::atomic<int> deactivations{0};
  wil::unique_event release{wil::EventOptions::ManualReset};
};

wchar_t* co_string(const wchar_t* text) {
  const std::size_t bytes = (std::wcslen(text) + 1) * sizeof(wchar_t);
  auto* copy = static_cast<wchar_t*>(::CoTaskMemAlloc(bytes));
  std::memcpy(copy, text, bytes);
  return copy;
}

class FakeHandler final : public Microsoft::WRL::RuntimeClass<
                              Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                              IEmptyVolumeCache2, IEmptyVolumeCache> {
 public:
  explicit FakeHandler(std::shared_ptr<Script> script) : script_(std::move(script)) {}

  HRESULT STDMETHODCALLTYPE InitializeEx(HKEY /*key*/, LPCWSTR /*volume*/, LPCWSTR /*name*/,
                                         LPWSTR* display, LPWSTR* description, LPWSTR* button,
                                         DWORD* flags) override {
    *button = nullptr;
    return Initialize(nullptr, nullptr, display, description, flags);
  }
  HRESULT STDMETHODCALLTYPE Initialize(HKEY /*key*/, LPCWSTR /*volume*/, LPWSTR* display,
                                       LPWSTR* description, DWORD* flags) override {
    script_->flags_in = *flags;
    *flags = EVCF_DONTSHOWIFZERO;
    *display = co_string(L"Fake Files");
    *description = nullptr;
    return script_->initialize;
  }
  HRESULT STDMETHODCALLTYPE GetSpaceUsed(DWORDLONG* used,
                                         IEmptyVolumeCacheCallBack* callback) override {
    ++script_->scans;
    *used = script_->space;
    switch (script_->scan) {
      case Scan::report:
        return S_OK;
      case Scan::block:
        ::WaitForSingleObject(script_->release.get(), INFINITE);
        return E_ABORT;
      case Scan::show_window: {
        const wil::unique_hwnd window(
            ::CreateWindowExW(0, L"STATIC", L"fake handler", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              window_size, window_size, nullptr, nullptr, nullptr, nullptr));
        return wait_for_abort(callback);
      }
      case Scan::wait_for_abort:
        return wait_for_abort(callback);
    }
    return E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE Purge(DWORDLONG to_free, IEmptyVolumeCacheCallBack* callback) override {
    ++script_->purges;
    return callback->PurgeProgress(to_free, 0, EVCCBF_LASTNOTIFICATION, nullptr);
  }
  HRESULT STDMETHODCALLTYPE ShowProperties(HWND /*window*/) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Deactivate(DWORD* flags) override {
    ++script_->deactivations;
    *flags = 0;
    return S_OK;
  }

 private:
  static HRESULT wait_for_abort(IEmptyVolumeCacheCallBack* callback) {
    while (callback->ScanProgress(0, 0, nullptr) != E_ABORT) {
      ::Sleep(pace_ms);  // paces the reports of a handler that is busy scanning
    }
    return E_ABORT;
  }

  std::shared_ptr<Script> script_;
};

class HandlerHostTest : public ::testing::Test {
 protected:
  HandlerLoader loader() {
    return [script = script_](const std::wstring& /*name*/) -> Result<LoadedHandler> {
      LoadedHandler loaded;
      loaded.clsid = "fake";
      const auto fake = Microsoft::WRL::Make<FakeHandler>(script);
      if (FAILED(fake.CopyTo(loaded.cache.put()))) {
        return fail(Errc::io_error, "fake handler not created");
      }
      return loaded;
    };
  }

  void TearDown() override { script_->release.SetEvent(); }

  Script& script() { return *script_; }

 private:
  std::shared_ptr<Script> script_ = std::make_shared<Script>();
};

TEST_F(HandlerHostTest, ScanReportsSpaceAndNeverPurges) {
  script().space = 4096;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const Result<Bytes> used = host.space_used("Fake", "C:\\", {});

  ASSERT_TRUE(used) << used.error().message;
  EXPECT_EQ(*used, Bytes{4096});
  EXPECT_EQ(script().flags_in, 0U);  // never EVCF_SETTINGSMODE: it deletes inside Initialize
  EXPECT_EQ(script().purges, 0);
  EXPECT_EQ(script().deactivations, 0);
}

TEST_F(HandlerHostTest, ProbeRecordsWhatTheHandlerAnswered) {
  script().space = 512;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const HandlerProbe probe = host.probe("Fake", "C:\\", {});

  EXPECT_TRUE(probe.loaded);
  EXPECT_TRUE(probe.initialize_ex);
  EXPECT_EQ(probe.display_name, "Fake Files");
  EXPECT_EQ(probe.flags, static_cast<std::uint32_t>(EVCF_DONTSHOWIFZERO));
  EXPECT_EQ(probe.space_used, Bytes{512});
  EXPECT_FALSE(probe.showed_window);
}

TEST_F(HandlerHostTest, NothingToCleanSkipsTheScan) {
  script().initialize = S_FALSE;
  script().space = 4096;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const Result<Bytes> used = host.space_used("Fake", "C:\\", {});

  ASSERT_TRUE(used);
  EXPECT_EQ(*used, Bytes{});
  EXPECT_EQ(script().scans, 0);
}

TEST_F(HandlerHostTest, PurgeScansPurgesAndDeactivates) {
  script().space = 4096;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const Result<void> purged = host.purge("Fake", "C:\\", {});

  ASSERT_TRUE(purged) << purged.error().message;
  EXPECT_EQ(script().flags_in, 0U);
  EXPECT_EQ(script().scans, 1);
  EXPECT_EQ(script().purges, 1);
  EXPECT_EQ(script().deactivations, 1);
}

TEST_F(HandlerHostTest, NothingFoundIsNotPurged) {
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  ASSERT_TRUE(host.purge("Fake", "C:\\", {}));
  EXPECT_EQ(script().purges, 0);
}

TEST_F(HandlerHostTest, HandlerThatShowsAWindowIsRefused) {
  script().scan = Scan::show_window;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const HandlerProbe probe = host.probe("Fake", "C:\\", {});
  const Result<void> status = probe_status(probe);

  EXPECT_TRUE(probe.showed_window);
  EXPECT_FALSE(probe.abandoned);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, Errc::refused);
}

TEST_F(HandlerHostTest, WindowDuringPurgeScanPreventsThePurge) {
  script().space = 4096;
  script().scan = Scan::show_window;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const Result<void> purged = host.purge("Fake", "C:\\", {});

  ASSERT_FALSE(purged);
  EXPECT_EQ(purged.error().code, Errc::refused);
  EXPECT_EQ(script().purges, 0);
}

TEST_F(HandlerHostTest, TimeoutAbortsThroughTheCallback) {
  script().scan = Scan::wait_for_abort;
  Win32HandlerHost host{milliseconds(200), seconds(10), loader()};

  const HandlerProbe probe = host.probe("Fake", "C:\\", {});
  const Result<void> status = probe_status(probe);

  EXPECT_TRUE(probe.timed_out);
  EXPECT_FALSE(probe.abandoned);
  EXPECT_EQ(probe.scan_result, E_ABORT);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code, Errc::io_error);
}

TEST_F(HandlerHostTest, StopRequestCancels) {
  script().scan = Scan::wait_for_abort;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};
  std::stop_source source;
  source.request_stop();

  const Result<Bytes> used = host.space_used("Fake", "C:\\", source.get_token());

  ASSERT_FALSE(used);
  EXPECT_EQ(used.error().code, Errc::refused);
}

TEST_F(HandlerHostTest, HandlerIgnoringTheAbortIsAbandoned) {
  script().scan = Scan::block;
  Win32HandlerHost host{milliseconds(100), seconds(10), loader()};

  const HandlerProbe probe = host.probe("Fake", "C:\\", {});

  EXPECT_TRUE(probe.timed_out);
  EXPECT_TRUE(probe.abandoned);
  EXPECT_FALSE(probe_status(probe));
}

TEST_F(HandlerHostTest, InitializeAccessDeniedMapsToAccessDenied) {
  script().initialize = E_ACCESSDENIED;
  Win32HandlerHost host{seconds(10), seconds(10), loader()};

  const Result<Bytes> used = host.space_used("Fake", "C:\\", {});

  ASSERT_FALSE(used);
  EXPECT_EQ(used.error().code, Errc::access_denied);
  EXPECT_EQ(script().scans, 0);
}

TEST(HandlerHostRegistry, UnknownHandlerIsNotFound) {
  Win32HandlerHost host{seconds(10), seconds(10)};

  const Result<Bytes> used = host.space_used("MyClean No Such Handler", "C:\\", {});

  ASSERT_FALSE(used);
  EXPECT_EQ(used.error().code, Errc::not_found);
}

TEST(HandlerHostRegistry, NameWithASeparatorIsRefused) {
  Win32HandlerHost host{seconds(10), seconds(10)};

  const Result<Bytes> used = host.space_used("..\\Temporary Files", "C:\\", {});

  ASSERT_FALSE(used);
  EXPECT_EQ(used.error().code, Errc::invalid_argument);
}

}  // namespace
}  // namespace myclean::platform
