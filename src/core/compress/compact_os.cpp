#include "core/compress/compact_os.hpp"

#include <filesystem>
#include <format>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/support.hpp"
#include "core/compress/compression.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/text.hpp"

namespace myclean::compress {

std::filesystem::path compact_os_item() {
  return {"compactos:system"};
}

CompactOs::CompactOs(cleanup::ICommandRunner& runner, const IWof& wof,
                     std::filesystem::path compact, Bytes estimate, std::stop_token stop)
    : runner_(&runner),
      wof_(&wof),
      compact_(std::move(compact)),
      estimate_(estimate),
      stop_(std::move(stop)) {}

ActionInfo CompactOs::describe() const {
  return {.what = "Compress the Windows system files (CompactOS)",
          .why =
              "Windows reads its own files compressed with XPRESS4K; turning it off again "
              "restores them",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::service};
}

Result<Preview> CompactOs::preview() {
  const Result<bool> on = wof_->compact_os();
  if (!on) {
    return tl::unexpected(on.error());
  }
  Preview preview;
  if (!*on) {
    preview.items.push_back(PlanItem{.path = compact_os_item(), .size = estimate_});
  }
  return preview;
}

Result<void> CompactOs::run(bool on) {
  const std::string_view mode = on ? "/compactos:always" : "/compactos:never";
  const Result<cleanup::CommandOutput> output = runner_->run(
      cleanup::Command{
          .program = space::utf8(compact_), .arguments = {std::string(mode)}, .timeout = timeout},
      stop_);
  if (!output) {
    return tl::unexpected(output.error());
  }
  const Result<bool> state = wof_->compact_os();
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (*state != on) {
    const std::vector<std::string_view> lines = cleanup::lines(output->out);
    return fail(Errc::io_error,
                std::format("compact {} left CompactOS {} (exit code {}){}{}", mode,
                            *state ? "on" : "off", output->exit_code, lines.empty() ? "" : ": ",
                            lines.empty() ? std::string_view{} : lines.front()));
  }
  return {};
}

Result<Undo> CompactOs::apply(const Preview& preview) {
  Undo undo;
  if (preview.items.empty()) {
    return undo;
  }
  if (Result<void> done = run(true); !done) {
    return tl::unexpected(done.error());
  }
  for (const PlanItem& item : preview.items) {
    undo.items.push_back({.item = item.id, .data = "compactos"});
  }
  return undo;
}

Result<Verification> CompactOs::verify(const Undo& undo) {
  const Result<bool> on = wof_->compact_os();
  if (!on) {
    return tl::unexpected(on.error());
  }
  // An empty undo means the preview found CompactOS on already.
  const double before = undo.items.empty() ? 1.0 : 0.0;
  const double after = *on ? 1.0 : 0.0;
  Verification verification{.metric = "compactos_on", .before = before, .after = after};
  if (after > before) {
    verification.effect = Effect::better;
  } else if (after < before) {
    verification.effect = Effect::worse;
  }
  return verification;
}

Result<void> CompactOs::undo(const Undo& undo) {
  if (undo.items.empty()) {
    return {};
  }
  return run(false);
}

}  // namespace myclean::compress
