#pragma once

#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"

// Compacting WSL and Docker virtual disks (optimizer spec, "VHDX Size Report and Compaction"):
// an ask-tier action the owner approves, which stops every WSL distribution and Docker Desktop's
// containers first. The Win32 side lives in src/platform/vhdx.
namespace myclean::vhdx {

// Compacts a virtual disk file. Implemented by src/platform/vhdx (CompactVirtualDisk).
class IVirtualDisks {
 public:
  virtual ~IVirtualDisks() = default;
  IVirtualDisks(const IVirtualDisks&) = delete;
  IVirtualDisks& operator=(const IVirtualDisks&) = delete;
  IVirtualDisks(IVirtualDisks&&) = delete;
  IVirtualDisks& operator=(IVirtualDisks&&) = delete;

  // Gives the unused blocks of the detached disk `file` back to its volume. Fails with in_use
  // while something has the disk attached or open, not_found when it is gone. May take long.
  [[nodiscard]] virtual Result<void> compact(const std::filesystem::path& file) = 0;

 protected:
  IVirtualDisks() = default;
};

// Stops WSL. Implemented by src/platform/vhdx (`wsl.exe --shutdown`, run as the owner).
class IWsl {
 public:
  virtual ~IWsl() = default;
  IWsl(const IWsl&) = delete;
  IWsl& operator=(const IWsl&) = delete;
  IWsl(IWsl&&) = delete;
  IWsl& operator=(IWsl&&) = delete;

  // Ends every running distribution and the WSL virtual machine, so their disks are detached.
  [[nodiscard]] virtual Result<void> shutdown() = 0;

 protected:
  IWsl() = default;
};

// The name plans of this action carry.
inline constexpr std::string_view compact_action_name = "CompactVhdx";

// Compacts the .vhdx files `disks`, run by the service, tier ask (architecture section 6).
//   preview  one item per disk (its identity and size); fails with invalid_argument for a file
//            that is not a .vhdx, a link, a cloud file, a folder or a disk named twice, and with
//            not_found for one that is gone;
//   apply    refuses (changing nothing) when a disk is gone or another file than the preview's;
//            then `wsl --shutdown` (a failure stops here, nothing is compacted), then each disk
//            is compacted. A disk still in use (Docker Desktop started it again) or failing is
//            skipped; when none could be compacted the first error is returned. Undo data: per
//            item {"path", "before", "after"} (sizes in bytes) or {"path", "before", "error"};
//            not reversible ("compaction only gives unused space back; the disk keeps its data");
//   verify   metric "vhdx_bytes": the compacted disks' sizes before and now; better when smaller,
//            unchanged when equal;
//   undo     always fails with not_reversible.
// Every reference must outlive the action.
class CompactVhdx final : public Action {
 public:
  CompactVhdx(const os::IFileSystem& file_system, IWsl& wsl, IVirtualDisks& disks,
              std::vector<std::filesystem::path> files)
      : file_system_(&file_system), wsl_(&wsl), disks_(&disks), files_(std::move(files)) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  const os::IFileSystem* file_system_;
  IWsl* wsl_;
  IVirtualDisks* disks_;
  std::vector<std::filesystem::path> files_;
};

}  // namespace myclean::vhdx
