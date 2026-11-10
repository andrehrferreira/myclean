#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/indexer/scope.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"

namespace myclean::indexer {

// The name plans of this action carry.
inline constexpr std::string_view exclusion_action_name = "SetIndexerScope";

// The program Windows Search's work is recorded under (resident::tree_key): SearchIndexer.exe,
// whose children are its protocol and filter hosts.
inline constexpr std::string_view indexer_program = "searchindexer.exe";

// What Windows Search's processes used over a span: CPU seconds, and the minutes they ran.
struct IndexerUse {
  double cpu_seconds = 0.0;
  std::chrono::minutes running{};
};

// The use of the stored hours that start in [from, to) (resident_hour, key indexer_program).
using IndexerLoad = std::function<Result<IndexerUse>(WallTime from, WallTime to)>;

// How the indexer's CPU time per hour it runs is compared before and after an exclusion: the
// hours before span up to load_span_before, the hours after up to load_span_after; each side
// needs load_min_running, and the hours after are complete at load_enough_running or when the
// span is over. The hour of the change itself counts on neither side. Indexer work varies with
// what the owner does, so differences within load_tolerance are unchanged.
inline constexpr std::chrono::days load_span_before{7};
inline constexpr std::chrono::days load_span_after{7};
inline constexpr std::chrono::minutes load_min_running{60};
inline constexpr std::chrono::hours load_enough_running{24};
inline constexpr double load_tolerance = 0.10;

// Keeps folders out of the Windows Search index (optimizer spec, "Search-Indexer Scope") with
// one user exclusion rule each; the folders and their files are not touched. Undo leaves the
// crawl scope exactly as it was: it removes each rule it added, or puts back the user rule of
// the same URL it replaced. Tier ask, run by the service; `scope` must outlive the action.
class IndexerExclusion final : public Action {
 public:
  // `load` and `clock` (may be empty and null: no load measurement) give the indexer's CPU time
  // an exclusion is verified with.
  IndexerExclusion(IIndexerScope& scope, std::vector<std::filesystem::path> folders,
                   IndexerLoad load = {}, const os::IClock* clock = nullptr)
      : scope_(&scope), folders_(std::move(folders)), load_(std::move(load)), clock_(clock) {}

  [[nodiscard]] ActionInfo describe() const override;
  // One item per folder, in the given order. Fails with invalid_argument for a folder listed
  // twice and with invalid_state for one the indexer does not crawl.
  [[nodiscard]] Result<Preview> preview() override;
  // Adds an exclusion rule for each item of `preview` the indexer still crawls; undo holds, per
  // folder changed, the user rule of that URL before (or none) and, with a clock, when it was
  // changed. When one change fails, those made are reversed and the error returned. Fails with
  // invalid_state when the indexer crawls none of them any more.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // First metric "folders_out_of_index": folders of `undo` the indexer no longer crawls, before
  // (0) and now; worse when one is crawled again. Then, with a load source, metric
  // "indexer_cpu_seconds_per_hour" (optimizer spec, "Before-and-After Verification"): the
  // indexer's CPU seconds per hour it ran before the change against after it (see load_span_*),
  // lower_is_better with load_tolerance; pending until the hours after are complete. When a side
  // has less than load_min_running, "folders_out_of_index" is better.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Restores the rule before of every folder of `undo`. Fails with invalid_state, changing
  // nothing, when a folder's rule is no longer the exclusion apply added.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  IIndexerScope* scope_;
  std::vector<std::filesystem::path> folders_;
  IndexerLoad load_;
  const os::IClock* clock_;
};

}  // namespace myclean::indexer
