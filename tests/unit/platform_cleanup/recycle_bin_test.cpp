#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>

#include "platform/cleanup/win32_recycle_bin.hpp"
// clang-format off
#include <shellapi.h>  // after windows.h
#include <shobjidl_core.h>
// clang-format on

#include "../cleanup/fakes.hpp"
#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The owner's $I records over a folder laid out like a volume. The shell's Recycle Bin of the user
// running the tests (listed read only, and one file of the test's own sent there and deleted
// again) only with MYCLEAN_TEST_RECYCLE_BIN=1, so that a test run never touches the real bin.
// Nothing shows a window: every shell call runs on a private desktop with FOF_NO_UI.
namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::wstring_view sid = L"S-1-5-21-1-2-3-1001";

// MYCLEAN_TEST_RECYCLE_BIN=1: the tests may use the real Recycle Bin.
bool real_bin_allowed() {
  std::size_t length = 0;
  std::array<char, 8> value{};
  return ::getenv_s(&length, value.data(), value.size(), "MYCLEAN_TEST_RECYCLE_BIN") == 0 &&
         std::string_view(value.data()) == "1";
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& data) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()),  // NOLINT(*-reinterpret-cast)
             static_cast<std::streamsize>(data.size()));
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class TempVolume {
 public:
  TempVolume()
      : root_(fs::temp_directory_path() /
              std::format(L"myclean-bin-{}-{}", ::GetCurrentProcessId(),
                          std::chrono::steady_clock::now().time_since_epoch().count())) {
    fs::create_directories(root_);
  }
  TempVolume(const TempVolume&) = delete;
  TempVolume& operator=(const TempVolume&) = delete;
  TempVolume(TempVolume&&) = delete;
  TempVolume& operator=(TempVolume&&) = delete;
  ~TempVolume() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  [[nodiscard]] const fs::path& root() const { return root_; }

 private:
  fs::path root_;
};

TEST(Win32BinRecords, ReadsTheOwnersRecordsAndWhetherTheirContentIsLeft) {
  const TempVolume volume;
  const fs::path bin = volume.root() / L"$Recycle.Bin" / sid;
  const WallTime deleted = std::chrono::sys_days{std::chrono::October / 1 / 2026} + 0h;
  const std::vector<std::uint8_t> kept =
      cleanup::testing::bin_record(2, 4096, deleted, LR"(E:\Build\a.rar)");
  const std::vector<std::uint8_t> gone =
      cleanup::testing::bin_record(1, 70, deleted, LR"(E:\old.txt)");
  write_file(bin / L"$IAB12CD.rar", kept);
  write_file(bin / L"$RAB12CD.rar", {1, 2, 3});
  write_file(bin / L"$IXY34ZW.txt", gone);
  write_file(bin / L"desktop.ini", {'x'});
  fs::create_directories(bin / L"$RFOLDER");
  write_file(volume.root() / L"$Recycle.Bin" / L"S-1-5-18" / L"$ISYSTEM", kept);
  const TempVolume empty;

  const Win32BinRecords records(std::wstring(sid), {volume.root(), empty.root()});
  const Result<std::vector<cleanup::BinFile>> files = records.read({});

  ASSERT_TRUE(files) << files.error().message;
  ASSERT_EQ(files->size(), 2U);
  const auto by_name = [&](std::wstring_view name) {
    return std::ranges::find(*files, bin / name, &cleanup::BinFile::path);
  };
  const auto first = by_name(L"$IAB12CD.rar");
  ASSERT_NE(first, files->end());
  EXPECT_TRUE(first->has_content);
  EXPECT_EQ(first->data, kept);
  const auto second = by_name(L"$IXY34ZW.txt");
  ASSERT_NE(second, files->end());
  EXPECT_FALSE(second->has_content);
  EXPECT_EQ(cleanup::aged_records(*files, deleted + 30 * 24h, std::chrono::days{30}).bytes,
            Bytes{4096});
}

TEST(Win32BinRecords, ListsOnlyFixedVolumes) {
  for (const fs::path& root : fixed_volumes()) {
    EXPECT_EQ(::GetDriveTypeW(root.c_str()), static_cast<UINT>(DRIVE_FIXED)) << root;
  }
  EXPECT_FALSE(fixed_volumes().empty());
}

// The shell's deletion date of each item is the one its $I record holds.
TEST(Win32RecycleBin, ListsTheBinReadOnlyWithTheRecordedDeletionTimes) {
  if (!real_bin_allowed()) {
    GTEST_SKIP() << "set MYCLEAN_TEST_RECYCLE_BIN=1 to read the real Recycle Bin";
  }
  Win32RecycleBin bin(2min);
  const Result<std::vector<cleanup::BinItem>> items = bin.items({});
  ASSERT_TRUE(items) << items.error().message;
  int compared = 0;
  for (const cleanup::BinItem& item : *items | std::views::take(20)) {
    const fs::path content = fs::path(std::u8string(item.id.begin(), item.id.end()));
    const std::wstring name = content.filename().wstring();
    ASSERT_TRUE(name.starts_with(L"$R")) << item.id;
    const fs::path record = content.parent_path() / (L"$I" + name.substr(2));
    const std::optional<cleanup::BinRecord> parsed = cleanup::parse_bin_record(read_bytes(record));
    if (!parsed) {
      continue;
    }
    EXPECT_LE(std::chrono::abs(parsed->deleted - item.deleted), 1s) << item.name;
    ++compared;
  }
  std::cout << std::format("{} items, {} compared with their $I record\n", items->size(), compared);
}

// Sends `file` to the Recycle Bin the way Explorer does, with no UI, on a private desktop.
bool recycle(const fs::path& file) {
  const std::wstring name = std::format(L"myclean-test-recycle-{}", ::GetCurrentProcessId());
  HDESK desktop = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  if (desktop == nullptr) {
    return false;
  }
  std::atomic<bool> recycled{false};
  std::thread thread([&] {
    if (::SetThreadDesktop(desktop) == FALSE ||
        FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
      return;
    }
    {
      wil::com_ptr_nothrow<IShellItem> item;
      wil::com_ptr_nothrow<IFileOperation> operation;
      recycled = SUCCEEDED(::SHCreateItemFromParsingName(file.c_str(), nullptr,
                                                         IID_PPV_ARGS(item.put()))) &&
                 SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(operation.put()))) &&
                 SUCCEEDED(operation->SetOperationFlags(
                     static_cast<DWORD>(FOF_NO_UI | FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE))) &&
                 SUCCEEDED(operation->DeleteItem(item.get(), nullptr)) &&
                 SUCCEEDED(operation->PerformOperations());
    }
    ::CoUninitialize();
  });
  thread.join();
  ::CloseDesktop(desktop);
  return recycled && !fs::exists(file);
}

// One file of the test's own goes to the bin and is then deleted permanently through the shell.
TEST(Win32RecycleBin, DeletesARecycledItemPermanently) {
  if (!real_bin_allowed()) {
    GTEST_SKIP() << "set MYCLEAN_TEST_RECYCLE_BIN=1 to recycle and delete a file of the test's own";
  }
  const fs::path file = fs::temp_directory_path() /
                        std::format(L"myclean-recycle-test-{}-{}.txt", ::GetCurrentProcessId(),
                                    std::chrono::steady_clock::now().time_since_epoch().count());
  if (::GetDriveTypeW(file.root_path().c_str()) != DRIVE_FIXED) {
    GTEST_SKIP() << "the temporary folder is not on a fixed volume";
  }
  write_file(file, {'b', 'i', 'n'});
  if (!recycle(file)) {
    fs::remove(file);
    GTEST_SKIP() << "the file could not be sent to the Recycle Bin";
  }
  Win32RecycleBin bin(2min);
  const auto ours = [&]() -> std::optional<cleanup::BinItem> {
    const Result<std::vector<cleanup::BinItem>> items = bin.items({});
    EXPECT_TRUE(items) << items.error().message;
    if (!items) {
      return std::nullopt;
    }
    // Explorer may hide the extension in the display name.
    const std::string stem = file.stem().string();
    const auto found = std::ranges::find_if(*items, [&](const cleanup::BinItem& item) {
      return item.name.find(stem) != std::string::npos;
    });
    return found == items->end() ? std::nullopt : std::optional(*found);
  };
  const std::optional<cleanup::BinItem> item = ours();
  if (!item) {
    GTEST_SKIP() << "the Recycle Bin of " << file.root_path() << " deleted the file at once";
  }
  EXPECT_LE(std::chrono::abs(std::chrono::system_clock::now() - item->deleted), 5min);
  EXPECT_EQ(item->size, Bytes{3});

  const std::vector<std::string> ids = {item->id, "C:\\$RECYCLE.BIN\\no-such-item"};
  const Result<std::vector<cleanup::Skipped>> left = bin.remove(ids, {});

  ASSERT_TRUE(left) << left.error().message;
  ASSERT_EQ(left->size(), 1U);
  EXPECT_EQ(left->front().error.code, Errc::not_found);
  EXPECT_FALSE(ours());
  EXPECT_FALSE(fs::exists(fs::path(std::u8string(item->id.begin(), item->id.end()))));
}

}  // namespace
}  // namespace myclean::platform
