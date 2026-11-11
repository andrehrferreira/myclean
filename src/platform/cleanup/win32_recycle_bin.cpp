#include "platform/cleanup/win32_recycle_bin.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>
// clang-format off
#include <knownfolders.h>  // after windows.h
#include <propsys.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
// clang-format on

#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/cleanup/wide.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

constexpr auto poll = std::chrono::milliseconds(50);
constexpr auto grace = std::chrono::seconds(30);  // for the item in progress after an abort

Error hresult_error(std::string_view what, HRESULT result) {
  const bool denied = result == E_ACCESSDENIED || result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  return {
      .code = denied ? Errc::access_denied : Errc::io_error,
      .message = std::format("{} failed with 0x{:08X}", what, static_cast<std::uint32_t>(result))};
}

Error last_error(std::string_view what) {
  return hresult_error(what, HRESULT_FROM_WIN32(::GetLastError()));
}

// "X:\..." on a fixed volume.
bool on_fixed_volume(std::wstring_view path) {
  if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') {
    return false;
  }
  const std::wstring root(path.substr(0, 3));
  return ::GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
}

// The `size` bytes of the file at `path`; nullopt when it cannot be read whole.
std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path, std::uint64_t size) {
  const wil::unique_hfile file(::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  DWORD read = 0;
  if (::ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) ==
          FALSE ||
      read != data.size()) {
    return std::nullopt;
  }
  return data;
}

// The keys the bin's items are read with, by their canonical names.
struct Keys {
  PROPERTYKEY deleted{};
  PROPERTYKEY deleted_from{};
  PROPERTYKEY size{};
};

Result<Keys> property_keys() {
  Keys keys;
  const std::array<std::pair<const wchar_t*, PROPERTYKEY*>, 3> names = {
      {{L"System.Recycle.DateDeleted", &keys.deleted},
       {L"System.Recycle.DeletedFrom", &keys.deleted_from},
       {L"System.Size", &keys.size}}};
  for (const auto& [name, key] : names) {
    if (const HRESULT found = ::PSGetPropertyKeyFromName(name, key); FAILED(found)) {
      return tl::unexpected(hresult_error("PSGetPropertyKeyFromName " + narrow(name), found));
    }
  }
  return keys;
}

// An item of the bin and what the owner sees of it.
struct Found {
  wil::com_ptr_nothrow<IShellItem2> item;
  cleanup::BinItem info;
};

// The items of the Recycle Bin folder on fixed volumes that have a deletion date. Fails with
// refused when `abort` is set before the end.
Result<std::vector<Found>> enumerate(const std::atomic<bool>& abort) {
  const Result<Keys> keys = property_keys();
  if (!keys) {
    return tl::unexpected(keys.error());
  }
  wil::com_ptr_nothrow<IShellItem> bin;
  if (const HRESULT opened = ::SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT,
                                                    nullptr, IID_PPV_ARGS(bin.put()));
      FAILED(opened)) {
    return tl::unexpected(hresult_error("SHGetKnownFolderItem RecycleBinFolder", opened));
  }
  wil::com_ptr_nothrow<IEnumShellItems> children;
  if (const HRESULT bound =
          bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(children.put()));
      FAILED(bound)) {
    return tl::unexpected(hresult_error("BindToHandler EnumItems", bound));
  }
  std::vector<Found> found;
  while (true) {
    if (abort) {
      return fail(Errc::refused, "cancelled");
    }
    wil::com_ptr_nothrow<IShellItem> child;
    if (children->Next(1, child.put(), nullptr) != S_OK) {
      break;
    }
    auto item = child.try_query<IShellItem2>();
    wil::unique_cotaskmem_string parsing;
    if (!item || FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, parsing.put())) ||
        !on_fixed_volume(parsing.get())) {
      continue;
    }
    FILETIME deleted{};
    if (FAILED(item->GetFileTime(keys->deleted, &deleted))) {
      continue;  // without a deletion time it is never old enough
    }
    ULONGLONG size = 0;
    if (FAILED(item->GetUInt64(keys->size, &size))) {
      size = 0;
    }
    std::wstring name;
    if (wil::unique_cotaskmem_string from;
        SUCCEEDED(item->GetString(keys->deleted_from, from.put()))) {
      name = std::wstring(from.get()) + L"\\";
    }
    if (wil::unique_cotaskmem_string display;
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, display.put()))) {
      name += display.get();
    }
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(deleted.dwHighDateTime) << 32U) | deleted.dwLowDateTime;
    found.push_back(Found{.item = std::move(item),
                          .info = {.id = narrow(parsing.get()),
                                   .name = narrow(name),
                                   .deleted = cleanup::from_filetime(ticks),
                                   .size = Bytes{size}}});
  }
  return found;
}

// Deletes `item` permanently: FOF_NO_UI, and neither FOF_ALLOWUNDO nor FOFX_RECYCLEONDELETE.
Result<void> delete_permanently(IShellItem& item) {
  wil::com_ptr_nothrow<IFileOperation> operation;
  if (const HRESULT created = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(operation.put()));
      FAILED(created)) {
    return tl::unexpected(hresult_error("CoCreateInstance FileOperation", created));
  }
  if (const HRESULT flags = operation->SetOperationFlags(static_cast<DWORD>(FOF_NO_UI));
      FAILED(flags)) {
    return tl::unexpected(hresult_error("IFileOperation::SetOperationFlags", flags));
  }
  if (const HRESULT queued = operation->DeleteItem(&item, nullptr); FAILED(queued)) {
    return tl::unexpected(hresult_error("IFileOperation::DeleteItem", queued));
  }
  if (const HRESULT performed = operation->PerformOperations(); FAILED(performed)) {
    return tl::unexpected(hresult_error("IFileOperation::PerformOperations", performed));
  }
  BOOL aborted = FALSE;
  if (SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted != FALSE) {
    return fail(Errc::io_error, "the shell did not delete it");
  }
  return {};
}

// A progress sink that lets a delete run only when the shell recycles the item: PreDeleteItem
// without TSF_DELETE_RECYCLE_IF_POSSIBLE (a volume without a Recycle Bin, an item too large for
// it) is a permanent delete, which it refuses. Renames, moves, copies and new items, which a
// delete never makes, are refused too.
class RecycleOnly final : public Microsoft::WRL::RuntimeClass<
                              Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                              IFileOperationProgressSink> {
 public:
  HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT /*result*/) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD /*flags*/, IShellItem* /*item*/,
                                          LPCWSTR /*name*/) override {
    return E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD /*flags*/, IShellItem* /*item*/, LPCWSTR /*name*/,
                                           HRESULT /*result*/, IShellItem* /*renamed*/) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD /*flags*/, IShellItem* /*item*/,
                                        IShellItem* /*folder*/, LPCWSTR /*name*/) override {
    return E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD /*flags*/, IShellItem* /*item*/,
                                         IShellItem* /*folder*/, LPCWSTR /*name*/,
                                         HRESULT /*result*/, IShellItem* /*moved*/) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD /*flags*/, IShellItem* /*item*/,
                                        IShellItem* /*folder*/, LPCWSTR /*name*/) override {
    return E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD /*flags*/, IShellItem* /*item*/,
                                         IShellItem* /*folder*/, LPCWSTR /*name*/,
                                         HRESULT /*result*/, IShellItem* /*copied*/) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem* /*item*/) override {
    if ((flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) == 0) {
      permanent_ = true;
      return E_ABORT;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD /*flags*/, IShellItem* /*item*/, HRESULT result,
                                           IShellItem* /*recycled*/) override {
    recycled_ = SUCCEEDED(result);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD /*flags*/, IShellItem* /*folder*/,
                                       LPCWSTR /*name*/) override {
    return E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD /*flags*/, IShellItem* /*folder*/, LPCWSTR /*name*/,
                                        LPCWSTR /*source*/, DWORD /*attributes*/,
                                        HRESULT /*result*/, IShellItem* /*item*/) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT /*total*/, UINT /*done*/) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

  [[nodiscard]] bool permanent() const { return permanent_; }
  [[nodiscard]] bool recycled() const { return recycled_; }

 private:
  bool permanent_ = false;
  bool recycled_ = false;
};

// Sends `file` to the Recycle Bin of its volume: FOF_NO_UI | FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE,
// with RecycleOnly watching, so that nothing is ever deleted permanently.
Result<void> send_to_bin(const fs::path& file) {
  wil::com_ptr_nothrow<IShellItem> item;
  if (const HRESULT parsed =
          ::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(item.put()));
      FAILED(parsed)) {
    Error error = hresult_error("SHCreateItemFromParsingName", parsed);
    if (parsed == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        parsed == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
      error.code = Errc::not_found;
    }
    return tl::unexpected(std::move(error));
  }
  wil::com_ptr_nothrow<IFileOperation> operation;
  if (const HRESULT created = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(operation.put()));
      FAILED(created)) {
    return tl::unexpected(hresult_error("CoCreateInstance FileOperation", created));
  }
  if (const HRESULT flags = operation->SetOperationFlags(
          static_cast<DWORD>(FOF_NO_UI | FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE));
      FAILED(flags)) {
    return tl::unexpected(hresult_error("IFileOperation::SetOperationFlags", flags));
  }
  const Microsoft::WRL::ComPtr<RecycleOnly> sink = Microsoft::WRL::Make<RecycleOnly>();
  if (sink.Get() == nullptr) {
    return fail(Errc::io_error, "out of memory for the progress sink");
  }
  if (const HRESULT queued = operation->DeleteItem(item.get(), sink.Get()); FAILED(queued)) {
    return tl::unexpected(hresult_error("IFileOperation::DeleteItem", queued));
  }
  const HRESULT performed = operation->PerformOperations();
  if (sink->permanent()) {
    return fail(Errc::refused, "the Recycle Bin of its volume cannot take it, so it stays");
  }
  if (FAILED(performed)) {
    return tl::unexpected(hresult_error("IFileOperation::PerformOperations", performed));
  }
  BOOL aborted = FALSE;
  if ((SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted != FALSE) ||
      !sink->recycled()) {
    return fail(Errc::io_error, "the shell did not recycle it");
  }
  return {};
}

// Shared by a call and its thread, which outlives the call when it is abandoned.
template <typename T>
struct CallState {
  std::mutex mutex;
  std::condition_variable done_changed;
  bool done = false;  // guarded by mutex; `result` is the thread's until then
  std::atomic<bool> abort{false};
  std::optional<Result<T>> result;
};

// Runs `body` on a fresh thread assigned to `desktop`, in a single-threaded apartment, and waits
// for it. Past `timeout`, or once `stop` is requested, `body` is told to abort and gets `grace`
// to end; then its thread is abandoned and the call fails.
template <typename T>
Result<T> isolated(HDESK desktop, std::chrono::milliseconds timeout, const std::stop_token& stop,
                   std::function<Result<T>(const std::atomic<bool>& abort)> body) {
  if (desktop == nullptr) {
    return fail(Errc::io_error, "no private desktop: the Recycle Bin is not touched without one");
  }
  auto state = std::make_shared<CallState<T>>();
  std::thread thread([state, desktop, body = std::move(body)] {
    Result<T> result = fail(Errc::io_error, "not run");
    if (::SetThreadDesktop(desktop) == FALSE) {
      result = tl::unexpected(last_error("SetThreadDesktop"));
    } else if (const HRESULT com =
                   ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
               FAILED(com)) {
      result = tl::unexpected(hresult_error("CoInitializeEx", com));
    } else {
      result = body(state->abort);
      ::CoUninitialize();
    }
    {
      const std::scoped_lock lock(state->mutex);
      state->result = std::move(result);
      state->done = true;
    }
    state->done_changed.notify_all();
  });
  const auto started = std::chrono::steady_clock::now();
  bool cancelled = false;
  std::unique_lock lock(state->mutex);
  while (!state->done_changed.wait_for(lock, poll, [&] { return state->done; })) {
    cancelled = stop.stop_requested();
    if (cancelled || std::chrono::steady_clock::now() - started >= timeout) {
      state->abort = true;
      state->done_changed.wait_for(lock, grace, [&] { return state->done; });
      break;
    }
  }
  if (!state->done) {
    lock.unlock();
    thread.detach();
    return fail(cancelled ? Errc::refused : Errc::io_error,
                std::string(cancelled ? "cancelled" : "timed out") +
                    "; the shell call in progress was left running");
  }
  Result<T> result = *std::move(state->result);
  lock.unlock();
  thread.join();
  return result;
}

}  // namespace

std::vector<fs::path> fixed_volumes() {
  std::vector<fs::path> roots;
  const DWORD drives = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    const std::wstring root{letter, L':', L'\\'};
    if ((drives & (1U << static_cast<unsigned>(letter - L'A'))) != 0 &&
        ::GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
      roots.emplace_back(root);
    }
  }
  return roots;
}

Win32BinRecords::Win32BinRecords(std::wstring owner_sid, std::vector<fs::path> volumes)
    : owner_sid_(std::move(owner_sid)), volumes_(std::move(volumes)) {}

Result<std::vector<cleanup::BinFile>> Win32BinRecords::read(const std::stop_token& stop) const {
  std::vector<cleanup::BinFile> files;
  for (const fs::path& volume : volumes_.empty() ? fixed_volumes() : volumes_) {
    const fs::path bin = volume / L"$Recycle.Bin" / owner_sid_;
    WIN32_FIND_DATAW entry{};
    const wil::unique_hfind find(::FindFirstFileExW((bin / L"$I*").c_str(), FindExInfoBasic, &entry,
                                                    FindExSearchNameMatch, nullptr,
                                                    FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
          error == ERROR_NOT_READY) {
        continue;
      }
      return tl::unexpected(
          hresult_error("FindFirstFileEx " + narrow(bin.wstring()), HRESULT_FROM_WIN32(error)));
    }
    for (bool more = true; more; more = ::FindNextFileW(find.get(), &entry) != FALSE) {
      if (stop.stop_requested()) {
        return fail(Errc::refused, "cancelled");
      }
      const std::wstring name{static_cast<const wchar_t*>(entry.cFileName)};
      const std::uint64_t size =
          (static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32U) | entry.nFileSizeLow;
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !name.starts_with(L"$I") ||
          size > cleanup::max_bin_record) {
        continue;
      }
      std::optional<std::vector<std::uint8_t>> data = read_file(bin / name, size);
      if (!data) {
        continue;  // gone since the listing, or locked
      }
      const fs::path content = bin / (L"$R" + name.substr(2));
      files.push_back(cleanup::BinFile{
          .path = bin / name,
          .data = *std::move(data),
          .has_content = ::GetFileAttributesW(content.c_str()) != INVALID_FILE_ATTRIBUTES});
    }
  }
  return files;
}

Win32RecycleBin::Win32RecycleBin(std::chrono::milliseconds timeout) : timeout_(timeout) {
  static std::atomic<int> count{0};
  const std::wstring name =
      std::format(L"myclean-recycle-bin-{}-{}", ::GetCurrentProcessId(), count++);
  desktop_ = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
}

Win32RecycleBin::~Win32RecycleBin() {
  if (desktop_ != nullptr) {
    ::CloseDesktop(desktop_);  // fails, leaving it open, while an abandoned thread still uses it
  }
}

Result<std::vector<cleanup::BinItem>> Win32RecycleBin::items(const std::stop_token& stop) {
  return isolated<std::vector<cleanup::BinItem>>(
      desktop_, timeout_, stop,
      [](const std::atomic<bool>& abort) -> Result<std::vector<cleanup::BinItem>> {
        Result<std::vector<Found>> found = enumerate(abort);
        if (!found) {
          return tl::unexpected(found.error());
        }
        std::vector<cleanup::BinItem> items;
        for (Found& entry : *found) {
          items.push_back(std::move(entry.info));
        }
        return items;
      });
}

Result<std::vector<cleanup::Skipped>> Win32RecycleBin::remove(std::span<const std::string> ids,
                                                              const std::stop_token& stop) {
  return isolated<std::vector<cleanup::Skipped>>(
      desktop_, timeout_, stop,
      [wanted_ids = std::vector<std::string>(ids.begin(), ids.end())](
          const std::atomic<bool>& abort) -> Result<std::vector<cleanup::Skipped>> {
        const Result<std::vector<Found>> found = enumerate(abort);
        if (!found) {
          return tl::unexpected(found.error());
        }
        std::set<std::string, std::less<>> wanted(wanted_ids.begin(), wanted_ids.end());
        std::vector<cleanup::Skipped> left;
        for (const Found& entry : *found) {
          if (wanted.erase(entry.info.id) == 0) {
            continue;
          }
          if (abort) {
            left.push_back({.target = entry.info.name,
                            .error = {.code = Errc::refused, .message = "cancelled"}});
          } else if (const Result<void> deleted = delete_permanently(*entry.item); !deleted) {
            left.push_back({.target = entry.info.name, .error = deleted.error()});
          }
        }
        for (const std::string& id : wanted) {
          left.push_back(
              {.target = id,
               .error = {.code = Errc::not_found, .message = "no longer in the Recycle Bin"}});
        }
        return left;
      });
}

Result<std::vector<cleanup::Skipped>> Win32RecycleBin::recycle(std::span<const fs::path> files,
                                                               const std::stop_token& stop) {
  return isolated<std::vector<cleanup::Skipped>>(
      desktop_, timeout_, stop,
      [wanted = std::vector<fs::path>(files.begin(), files.end())](
          const std::atomic<bool>& abort) -> Result<std::vector<cleanup::Skipped>> {
        std::vector<cleanup::Skipped> left;
        for (const fs::path& file : wanted) {
          const std::string target = narrow(file.wstring());
          if (abort) {
            left.push_back(
                {.target = target, .error = {.code = Errc::refused, .message = "cancelled"}});
          } else if (const Result<void> sent = send_to_bin(file); !sent) {
            left.push_back({.target = target, .error = sent.error()});
          }
        }
        return left;
      });
}

}  // namespace myclean::platform
