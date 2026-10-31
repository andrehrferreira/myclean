#include "core/cleanup/handler_cleanup.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/cleanup/handler_host.hpp"
#include "core/cleanup/support.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/safety/paths.hpp"

namespace myclean::cleanup {

namespace {

constexpr std::string_view item_prefix = "handler:";

}  // namespace

std::filesystem::path handler_item(std::string_view name) {
  return from_utf8(std::string(item_prefix) + std::string(name));
}

HandlerCleanup::HandlerCleanup(IHandlerHost& host, std::filesystem::path volume,
                               std::vector<std::string> handlers, Tier tier,
                               ExecutionContext context, std::stop_token stop)
    : host_(&host),
      volume_(std::move(volume)),
      handlers_(std::move(handlers)),
      tier_(tier),
      context_(context),
      stop_(std::move(stop)) {}

ActionInfo HandlerCleanup::describe() const {
  std::string names;
  for (const std::string& name : handlers_) {
    names += (names.empty() ? "" : ", ") + name;
  }
  return {.what = "Run the Windows cleanup handlers " + names + " on " + safety::to_utf8(volume_),
          .why = "Windows removes its own files through its handlers",
          .risk = Risk::low,
          .tier = tier_,
          .context = context_};
}

Result<Preview> HandlerCleanup::preview() {
  skipped_.clear();
  Preview preview;
  for (const std::string& name : handlers_) {
    const Result<Bytes> used = host_->space_used(name, volume_, stop_);
    if (!used) {
      skipped_.push_back({.target = name, .error = used.error()});
    } else if (*used != Bytes{}) {
      preview.items.push_back(PlanItem{.path = handler_item(name), .size = *used});
    }
  }
  if (!handlers_.empty() && skipped_.size() == handlers_.size()) {
    return tl::unexpected(skipped_.front().error);
  }
  return preview;
}

Result<Undo> HandlerCleanup::apply(const Preview& preview) {
  skipped_.clear();
  before_ = Bytes{};
  Undo undo{.not_reversible = "regenerates"};
  for (const PlanItem& item : preview.items) {
    const std::string target = safety::to_utf8(item.path);
    std::optional<std::string> name;
    if (target.starts_with(item_prefix) &&
        std::ranges::find(handlers_, target.substr(item_prefix.size())) != handlers_.end()) {
      name = target.substr(item_prefix.size());
    }
    if (!name) {
      skipped_.push_back(
          {.target = target,
           .error = {.code = Errc::refused, .message = "not a handler of this action"}});
      continue;
    }
    if (stop_.stop_requested()) {
      skipped_.push_back(
          {.target = *name, .error = {.code = Errc::refused, .message = "cancelled"}});
      continue;
    }
    if (const Result<void> purged = host_->purge(*name, volume_, stop_); !purged) {
      skipped_.push_back({.target = *name, .error = purged.error()});
      continue;
    }
    before_ += item.size;
    undo.items.push_back({.item = item.id, .data = *name});
  }
  if (undo.items.empty() && !skipped_.empty()) {
    return tl::unexpected(skipped_.front().error);
  }
  return undo;
}

Result<Verification> HandlerCleanup::verify(const Undo& undo) {
  Bytes after;
  for (const UndoItem& item : undo.items) {
    const Result<Bytes> used = host_->space_used(item.data, volume_, stop_);
    if (!used) {
      return tl::unexpected(used.error());
    }
    after += *used;
  }
  return compare("handler_bytes_reclaimable", before_, after);
}

Result<void> HandlerCleanup::undo(const Undo& undo) {
  return fail(Errc::not_reversible, "not reversible: " + undo.not_reversible);
}

}  // namespace myclean::cleanup
