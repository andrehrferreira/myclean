#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>

#include "core/cleanup/command_runner.hpp"
#include "core/compress/compression.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::compress {

// The plan item path that stands for the Windows system files: "compactos:system".
[[nodiscard]] std::filesystem::path compact_os_item();

// Turns CompactOS on with `compact /compactos:always` and off again with `/compactos:never`
// (compression spec, "CompactOS"): Windows compresses its own files with XPRESS4K. Tier ask,
// never automatic; run by the service, which has the rights. compact.exe prints localized text,
// so the outcome is judged by the state IWof::compact_os() reads afterwards, never by the
// text. Every reference must outlive the action.
class CompactOs final : public Action {
 public:
  // Compressing or expanding every system file takes long.
  static constexpr std::chrono::milliseconds timeout{std::chrono::hours(3)};

  // `compact` is the absolute path of compact.exe (platform::compact_path()); `estimate` the
  // expected savings shown in the preview (an analyzer estimate of %WINDIR% with XPRESS4K).
  CompactOs(cleanup::ICommandRunner& runner, const IWof& wof, std::filesystem::path compact,
            Bytes estimate, std::stop_token stop = {});

  [[nodiscard]] ActionInfo describe() const override;
  // Empty when CompactOS is on already; otherwise one item, compact_os_item(), with the estimate.
  [[nodiscard]] Result<Preview> preview() override;
  // Runs `compact /compactos:always` unless the preview is empty. Fails with io_error, naming
  // the exit code, when CompactOS is not on afterwards. Undo lists the item (data: "compactos").
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "compactos_on": 0 before, 1 when it is on now.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Runs `compact /compactos:never` when the undo lists the item; fails with io_error when
  // CompactOS is still on afterwards.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  [[nodiscard]] Result<void> run(bool on);

  cleanup::ICommandRunner* runner_;
  const IWof* wof_;
  std::filesystem::path compact_;
  Bytes estimate_;
  std::stop_token stop_;
};

}  // namespace myclean::compress
