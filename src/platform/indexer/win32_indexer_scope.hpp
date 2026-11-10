#pragma once

#include <filesystem>
#include <vector>

#include "core/indexer/scope.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// The crawl scope of the Windows Search catalog "SystemIndex" through ISearchManager and
// ISearchCrawlScopeManager (the SearchIndexer service, out of process). Every call initializes
// COM for its thread (multithreaded, or the apartment the thread already has) and fails with
// io_error, naming the call, when Windows Search is not running. Changes are saved with SaveAll
// at once. Thread-safe: each call has its own COM objects.
class Win32IndexerScope final : public indexer::IIndexerScope {
 public:
  Win32IndexerScope() = default;

  [[nodiscard]] Result<std::vector<indexer::ScopeRule>> rules() const override;
  [[nodiscard]] Result<bool> included(const std::filesystem::path& folder) const override;
  [[nodiscard]] Result<void> set_rule(const std::filesystem::path& folder, bool include) override;
  [[nodiscard]] Result<void> remove_rule(const std::filesystem::path& folder) override;
};

}  // namespace myclean::platform
