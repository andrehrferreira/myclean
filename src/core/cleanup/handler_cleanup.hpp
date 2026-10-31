#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/cleanup/handler_host.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::cleanup {

// A target the last preview() or apply() left out, and why.
struct Skipped {
  std::string target;
  Error error;
};

// The plan item path that stands for handler `name`: "handler:<name>". It is not a file system
// path, so SafeRemove refuses it.
[[nodiscard]] std::filesystem::path handler_item(std::string_view name);

// Cleans Windows areas through their own disk-cleanup handlers (cleaning spec, "Windows System
// Areas Through Windows Handlers"). MyClean deletes nothing itself. Not reversible: what the
// handlers remove is regenerated when needed. `context` is the rule's scope: handlers of
// system areas run in the service, those that read the caller's own profile (Temporary Files,
// Thumbnail Cache, Internet Cache Files, D3D Shader Cache) in the tray; see
// docs/spikes/X3-cleanup-handlers.md. Every reference must outlive the action.
class HandlerCleanup final : public Action {
 public:
  HandlerCleanup(IHandlerHost& host, std::filesystem::path volume,
                 std::vector<std::string> handlers, Tier tier, ExecutionContext context,
                 std::stop_token stop = {});

  [[nodiscard]] ActionInfo describe() const override;
  // One item per handler that reports bytes to free; a handler that fails, shows a window or
  // reports nothing is left out (failures in skipped()). Fails when every handler failed.
  [[nodiscard]] Result<Preview> preview() override;
  // Purges the handler of each item, if it is one of this action's handlers. Undo lists each
  // purged handler (data: its name); not_reversible is "regenerates". Fails when items were
  // given and none could be purged.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "handler_bytes_reclaimable": the purged handlers' preview sizes against a new scan.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Always fails with not_reversible: "not reversible: regenerates".
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

  [[nodiscard]] const std::vector<Skipped>& skipped() const { return skipped_; }

 private:
  IHandlerHost* host_;
  std::filesystem::path volume_;
  std::vector<std::string> handlers_;
  Tier tier_;
  ExecutionContext context_;
  std::stop_token stop_;
  std::vector<Skipped> skipped_;
  Bytes before_;
};

}  // namespace myclean::cleanup
