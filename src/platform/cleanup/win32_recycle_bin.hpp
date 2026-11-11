#pragma once

#include <chrono>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <windows.h>

#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// The roots of the fixed volumes ("C:\", "D:\", ...), by letter.
[[nodiscard]] std::vector<std::filesystem::path> fixed_volumes();

// The owner's $I records, read by the service (LocalSystem reads every user's bin):
// <volume>$Recycle.Bin\<owner SID>\$I*, each with whether its $R content is still there. A volume
// without that folder, or not ready, adds nothing. Reads only.
class Win32BinRecords final : public cleanup::IBinRecords {
 public:
  // `volumes` empty: every fixed volume, listed again at each read (tests pass folders).
  explicit Win32BinRecords(std::wstring owner_sid, std::vector<std::filesystem::path> volumes = {});

  [[nodiscard]] Result<std::vector<cleanup::BinFile>> read(
      const std::stop_token& stop) const override;

 private:
  std::wstring owner_sid_;
  std::vector<std::filesystem::path> volumes_;
};

// The Recycle Bin of the user running this process (the owner, in the tray), through the shell's
// Recycle Bin folder (FOLDERID_RecycleBinFolder): items with System.Recycle.DateDeleted, deleted
// one IFileOperation each with FOF_NO_UI and without FOFX_RECYCLEONDELETE, so permanently and
// with no confirmation, progress or error UI, no owner window, no focus change and no clipboard.
// Every call also runs on a fresh single-threaded-apartment thread assigned to a private desktop
// of this process, so a window the shell might open anyway is never shown to the user. A call
// that exceeds `timeout`, or whose `stop` is requested, stops before the next item and fails;
// when the item in progress does not end within a grace period, its thread is abandoned and
// keeps running in the background.
//
// It is also the owner's cleanup::IRecycler: recycle() sends files to their volume's Recycle Bin
// the way Explorer's Delete does (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE), with the same private
// desktop, timeout and cancellation, and a progress sink that refuses the permanent delete the
// shell falls back to when the Recycle Bin cannot take a file, so that file stays.
class Win32RecycleBin final : public cleanup::IRecycleBin, public cleanup::IRecycler {
 public:
  explicit Win32RecycleBin(std::chrono::milliseconds timeout);
  ~Win32RecycleBin() override;
  Win32RecycleBin(const Win32RecycleBin&) = delete;
  Win32RecycleBin& operator=(const Win32RecycleBin&) = delete;
  Win32RecycleBin(Win32RecycleBin&&) = delete;
  Win32RecycleBin& operator=(Win32RecycleBin&&) = delete;

  [[nodiscard]] Result<std::vector<cleanup::BinItem>> items(const std::stop_token& stop) override;
  [[nodiscard]] Result<std::vector<cleanup::Skipped>> remove(std::span<const std::string> ids,
                                                             const std::stop_token& stop) override;
  [[nodiscard]] Result<std::vector<cleanup::Skipped>> recycle(
      std::span<const std::filesystem::path> files, const std::stop_token& stop) override;

 private:
  std::chrono::milliseconds timeout_;
  HDESK desktop_ = nullptr;
};

}  // namespace myclean::platform
