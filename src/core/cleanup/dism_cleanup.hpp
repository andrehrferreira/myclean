#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>

#include "core/cleanup/command_runner.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::cleanup {

// What `DISM /Online /Cleanup-Image /AnalyzeComponentStore /English` reports.
struct ComponentStore {
  Bytes actual_size;
  Bytes backups_and_disabled_features;
  Bytes cache_and_temporary_data;
  int reclaimable_packages = 0;
  bool cleanup_recommended = false;

  // The component store overhead (Microsoft: backups and disabled features plus cache and
  // temporary data): the most a cleanup can free.
  [[nodiscard]] Bytes overhead() const {
    return backups_and_disabled_features + cache_and_temporary_data;
  }
};

// Parses the English output of /AnalyzeComponentStore. Fails with invalid_data when a field is
// missing or malformed.
[[nodiscard]] Result<ComponentStore> parse_component_store(std::string_view output);

// The meaning of a DISM exit code, never of its localized text: 0 and 3010 (restart required)
// succeed; 740 and access denied fail with access_denied; 0x800F0806 (servicing operations
// pending) with in_use; 1223 (cancelled) with refused; any other with io_error.
[[nodiscard]] Result<void> dism_status(std::uint32_t exit_code);

// The plan item path that stands for the component store: "dism:component-store".
[[nodiscard]] std::filesystem::path component_store_item();

// Removes superseded components with `DISM /Online /Cleanup-Image /StartComponentCleanup`,
// run by the service (cleaning spec, "Windows System Areas Through Windows Handlers"). Never
// /ResetBase. Not reversible. The runner must outlive the action.
class DismCleanup final : public Action {
 public:
  static constexpr std::chrono::milliseconds analyze_timeout{std::chrono::minutes(30)};
  // Ending dism.exe does not stop the servicing it started, so the limit is generous.
  static constexpr std::chrono::milliseconds cleanup_timeout{std::chrono::hours(3)};

  // `dism` is the absolute path of dism.exe (platform::dism_path()).
  DismCleanup(ICommandRunner& runner, std::filesystem::path dism, Tier tier,
              std::stop_token stop = {});

  [[nodiscard]] ActionInfo describe() const override;
  // One item, the component store with its overhead, when DISM recommends a cleanup; empty
  // otherwise.
  [[nodiscard]] Result<Preview> preview() override;
  // Runs StartComponentCleanup unless the preview is empty. Undo lists the item (data: "dism");
  // not_reversible is "superseded components are removed".
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "component_store_overhead_bytes": the applied preview against a new analysis.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Always fails with not_reversible.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  [[nodiscard]] Result<ComponentStore> analyze() const;

  ICommandRunner* runner_;
  std::filesystem::path dism_;
  Tier tier_;
  std::stop_token stop_;
  Bytes before_;
};

}  // namespace myclean::cleanup
