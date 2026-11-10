#pragma once

#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include "core/indexer/scope.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"

namespace myclean::indexer {

// The name plans of this action carry.
inline constexpr std::string_view exclusion_action_name = "SetIndexerScope";

// Keeps folders out of the Windows Search index (optimizer spec, "Search-Indexer Scope") with
// one user exclusion rule each; the folders and their files are not touched. Undo leaves the
// crawl scope exactly as it was: it removes each rule it added, or puts back the user rule of
// the same URL it replaced. Tier ask, run by the service; `scope` must outlive the action.
class IndexerExclusion final : public Action {
 public:
  IndexerExclusion(IIndexerScope& scope, std::vector<std::filesystem::path> folders)
      : scope_(&scope), folders_(std::move(folders)) {}

  [[nodiscard]] ActionInfo describe() const override;
  // One item per folder, in the given order. Fails with invalid_argument for a folder listed
  // twice and with invalid_state for one the indexer does not crawl.
  [[nodiscard]] Result<Preview> preview() override;
  // Adds an exclusion rule for each item of `preview` the indexer still crawls; undo holds, per
  // folder changed, the user rule of that URL before (or none). When one change fails, those
  // made are reversed and the error returned. Fails with invalid_state when the indexer crawls
  // none of them any more.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "folders_out_of_index": folders of `undo` the indexer no longer crawls, before (0)
  // and now; better when all of them.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Restores the rule before of every folder of `undo`. Fails with invalid_state, changing
  // nothing, when a folder's rule is no longer the exclusion apply added.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  IIndexerScope* scope_;
  std::vector<std::filesystem::path> folders_;
};

}  // namespace myclean::indexer
