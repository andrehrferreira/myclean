#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"

// The system settings that hold space on the system drive: hibernation, page file and shadow
// storage (docs/analysis/myclean-planning/02-disk-cleaning-and-relocation.md, F-015). Each is an
// approval-only action (tier ask, run by the service) that shows its cost, the exact command it
// runs and its undo; propose() keeps them out of automatic approval.
namespace myclean::placement {

// What the Optimizations view shows for one change, before it is approved.
struct SettingSummary {
  std::string current;                     // "full (hiberfil.sys 50.9 GB)"
  std::string proposed;                    // "reduced"
  Bytes cost;                              // what the setting holds on disk now
  std::vector<std::string> commands;       // exactly what apply runs
  std::vector<std::string> undo_commands;  // exactly what undo runs
  bool restart_required = false;           // takes effect only after a restart
};

// Windows' hibernation file size for `mode`: 20 % of memory for reduced, `size_percent` (default
// 40 %) for full, nothing when off.
[[nodiscard]] Bytes hibernation_size(Bytes memory, HibernationMode mode,
                                     std::uint32_t size_percent);

// The powercfg commands that take hibernation from `from` to `mode` (with `size_percent` for
// full, 0 = leave the size alone): "powercfg /h off"; or "powercfg /h on" when it is off, then
// "powercfg /h /type reduced|full" and "powercfg /h /size N". Empty when nothing changes.
[[nodiscard]] std::vector<Command> hibernation_commands(const HibernationState& from,
                                                        HibernationMode mode,
                                                        std::uint32_t size_percent);

// "reduced" when hibernation is full (keeps Fast Startup, halves hiberfil.sys); nullopt else.
[[nodiscard]] std::optional<HibernationMode> recommended_hibernation(const HibernationState& state);

// Sets the hibernation type with powercfg. Undo restores the previous mode and size.
class HibernationAction final : public Action {
 public:
  // Both references must outlive the action.
  HibernationAction(ISystemSpace& system, IProcessRunner& runner, HibernationMode mode)
      : system_(&system), runner_(&runner), mode_(mode) {}

  [[nodiscard]] Result<SettingSummary> summary() const;
  [[nodiscard]] ActionInfo describe() const override;
  // One item: the hibernation file and its size.
  [[nodiscard]] Result<Preview> preview() override;
  // Fails with invalid_state when hibernation already is in the mode. When a command fails, the
  // ones that ran are reversed.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "hiberfil_bytes": the file's size before and now; smaller is better.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Fails with invalid_state, changing nothing, when the mode was changed meanwhile.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  ISystemSpace* system_;
  IProcessRunner* runner_;
  HibernationMode mode_;
};

// The steps that change the page-file configuration `from` into `to`, in the order the
// platform applies them through WMI: AutomaticManagedPagefile off, settings deleted, settings
// created or resized, AutomaticManagedPagefile on.
[[nodiscard]] std::vector<std::string> page_file_steps(const PageFileConfig& from,
                                                       const PageFileConfig& to);

// When a page file on the system drive is system-managed or larger than 4096 MB: a 4096 MB page
// file stays on the system drive (so that crash dumps can still be written) and a
// system-managed one goes to the best volume for the current page-file size (best_target).
// nullopt when the configuration is already like that or no volume qualifies.
[[nodiscard]] std::optional<PageFileConfig> recommended_page_files(
    const PageFileState& state, std::span<const TargetVolume> volumes,
    const std::filesystem::path& system_root);

// Changes the page-file configuration (Win32_PageFileSetting). It takes effect after a restart;
// undo restores the previous configuration, system-managed sizing included.
class PageFileAction final : public Action {
 public:
  // Both references must outlive the action.
  PageFileAction(ISystemSpace& system, const IDriveFacts& drives, PageFileConfig config)
      : system_(&system), drives_(&drives), config_(std::move(config)) {}

  [[nodiscard]] Result<SettingSummary> summary() const;
  [[nodiscard]] ActionInfo describe() const override;
  // One item per page file in use, with its allocated size.
  [[nodiscard]] Result<Preview> preview() override;
  // Refuses a page file on a volume that is not healthy or lacks room (unsuitable()), and fails
  // with invalid_state when the configuration already is `config`.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "page_file_settings_applied": 1 when the configuration is the new one, else 0.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Fails with invalid_state, changing nothing, when the configuration was changed meanwhile.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  ISystemSpace* system_;
  const IDriveFacts* drives_;
  PageFileConfig config_;
};

// The smallest size vssadmin accepts for a shadow storage.
inline constexpr Bytes minimum_shadow_storage{320ULL << 20U};

// "vssadmin resize shadowstorage /for=C: /on=C: /maxsize=<bytes|UNBOUNDED>".
[[nodiscard]] Command shadow_storage_command(const ShadowStorage& storage,
                                             std::optional<Bytes> maximum);

// 5 % of the volume, at least 10 GiB, when the current limit is above that; nullopt otherwise.
[[nodiscard]] std::optional<Bytes> recommended_shadow_maximum(const ShadowStorage& storage,
                                                              Bytes volume_size);

// Sets the maximum shadow storage of one volume with vssadmin (nullopt = unbounded). Shrinking
// deletes the oldest restore points that no longer fit; undo restores the limit, not them.
class ShadowStorageAction final : public Action {
 public:
  // Both references must outlive the action. `volume` as ShadowStorage::volume names it.
  ShadowStorageAction(ISystemSpace& system, IProcessRunner& runner, std::string volume,
                      std::optional<Bytes> maximum)
      : system_(&system), runner_(&runner), volume_(std::move(volume)), maximum_(maximum) {}

  [[nodiscard]] Result<SettingSummary> summary() const;
  [[nodiscard]] ActionInfo describe() const override;
  // One item: the volume's "System Volume Information" folder with the storage in use.
  [[nodiscard]] Result<Preview> preview() override;
  // Fails with invalid_argument below minimum_shadow_storage and with invalid_state when the
  // limit already is the requested one.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "shadow_storage_maximum" (bytes, -1 = unbounded): the limit before and now; better
  // when it is the new one.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Fails with invalid_state, changing nothing, when the limit was changed meanwhile.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  [[nodiscard]] Result<ShadowStorage> storage() const;

  ISystemSpace* system_;
  IProcessRunner* runner_;
  std::string volume_;
  std::optional<Bytes> maximum_;
};

}  // namespace myclean::placement
