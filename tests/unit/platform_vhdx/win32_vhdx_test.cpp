#include "platform/vhdx/win32_vhdx.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <initguid.h>
#include <virtdisk.h>

#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

// A fresh folder %TEMP%\myclean-it\<unique>, removed with everything in it afterwards.
class Sandbox {
 public:
  Sandbox()
      : root_(fs::temp_directory_path() / "myclean-it" /
              ("vhdx-" + std::to_string(::GetCurrentProcessId()) + "-" +
               std::to_string(::GetTickCount64()))) {
    fs::create_directories(root_);
  }
  ~Sandbox() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;
  Sandbox(Sandbox&&) = delete;
  Sandbox& operator=(Sandbox&&) = delete;

  [[nodiscard]] const fs::path& root() const { return root_; }

 private:
  fs::path root_;
};

// Creates a 64 MiB dynamically expanding VHDX at `file`: a file of a few MiB, never attached.
DWORD create_disk(const fs::path& file) {
  VIRTUAL_STORAGE_TYPE type{.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                            .VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
  CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
  parameters.Version = CREATE_VIRTUAL_DISK_VERSION_1;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): Version says which member is valid
  parameters.Version1.MaximumSize = std::uint64_t{64} << 20U;
  wil::unique_handle disk;
  return ::CreateVirtualDisk(&type, file.c_str(), VIRTUAL_DISK_ACCESS_ALL, nullptr,
                             CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, disk.put());
}

// The size of `file` in bytes, read through Win32.
std::uint64_t size_of(const fs::path& file) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) == FALSE) {
    return 0;
  }
  return (std::uint64_t{data.nFileSizeHigh} << 32U) | data.nFileSizeLow;
}

TEST(Win32VirtualDisks, CompactsADetachedDisk) {
  const Sandbox sandbox;
  const fs::path disk = sandbox.root() / "test.vhdx";
  ASSERT_EQ(create_disk(disk), static_cast<DWORD>(ERROR_SUCCESS));
  const std::uint64_t before = size_of(disk);
  EXPECT_GT(before, 0U);

  Win32VirtualDisks disks;
  const Result<void> compacted = disks.compact(disk);

  ASSERT_TRUE(compacted.has_value()) << compacted.error().message;
  EXPECT_LE(size_of(disk), before);
}

TEST(Win32VirtualDisks, ADiskHeldOpenIsInUseAndAMissingOneNotFound) {
  const Sandbox sandbox;
  const fs::path disk = sandbox.root() / "held.vhdx";
  ASSERT_EQ(create_disk(disk), static_cast<DWORD>(ERROR_SUCCESS));
  Win32VirtualDisks disks;
  {
    const wil::unique_hfile held{::CreateFileW(disk.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                               nullptr, OPEN_EXISTING, 0, nullptr)};
    ASSERT_TRUE(held);
    const Result<void> in_use = disks.compact(disk);
    ASSERT_FALSE(in_use.has_value());
    EXPECT_EQ(in_use.error().code, Errc::in_use) << in_use.error().message;
  }
  const Result<void> missing = disks.compact(sandbox.root() / "gone.vhdx");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

// Only where wsl.exe is looked for; WSL is never started or stopped by the tests.
TEST(WslPath, NamesAnExistingWslExeOrNone) {
  const std::optional<fs::path> wsl = wsl_path();
  if (wsl) {
    EXPECT_NE(::GetFileAttributesW(wsl->c_str()), INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(wsl->filename(), fs::path(L"wsl.exe"));
  }
}

}  // namespace
}  // namespace myclean::platform
