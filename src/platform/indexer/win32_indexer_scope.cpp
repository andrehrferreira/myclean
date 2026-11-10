#include "platform/indexer/win32_indexer_scope.hpp"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>
// windows.h first.
#include <searchapi.h>

#include "core/indexer/scope.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

tl::unexpected<Error> com_error(std::string_view what, HRESULT result) {
  const Errc code = result == E_ACCESSDENIED ? Errc::access_denied : Errc::io_error;
  return fail(code, std::format("{} failed (0x{:08X})", what, static_cast<unsigned>(result)));
}

// COM for the calling thread for the lifetime of the scope, in whatever apartment the thread
// already has (RPC_E_CHANGED_MODE: it keeps it and is not uninitialized here).
class ComScope {
 public:
  ComScope() : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)) {}
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  ComScope(ComScope&&) = delete;
  ComScope& operator=(ComScope&&) = delete;
  ~ComScope() {
    if (SUCCEEDED(result_)) {
      ::CoUninitialize();
    }
  }

  [[nodiscard]] Result<void> status() const {
    if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
      return com_error("CoInitializeEx", result_);
    }
    return {};
  }

 private:
  HRESULT result_;
};

// The crawl-scope manager of catalog SystemIndex.
Result<wil::com_ptr_nothrow<ISearchCrawlScopeManager>> crawl_scope() {
  wil::com_ptr_nothrow<ISearchManager> manager;
  if (const HRESULT created = ::CoCreateInstance(__uuidof(CSearchManager), nullptr,
                                                 CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
      FAILED(created)) {
    return com_error("Windows Search (CSearchManager)", created);
  }
  wil::com_ptr_nothrow<ISearchCatalogManager> catalog;
  if (const HRESULT got = manager->GetCatalog(L"SystemIndex", &catalog); FAILED(got)) {
    return com_error("ISearchManager::GetCatalog(SystemIndex)", got);
  }
  wil::com_ptr_nothrow<ISearchCrawlScopeManager> scope;
  if (const HRESULT got = catalog->GetCrawlScopeManager(&scope); FAILED(got)) {
    return com_error("ISearchCatalogManager::GetCrawlScopeManager", got);
  }
  return scope;
}

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

// Runs `work` on the crawl-scope manager inside a COM scope.
template <typename T, typename Work>
Result<T> with_scope(Work work) {
  const ComScope com;
  if (Result<void> ready = com.status(); !ready) {
    return tl::unexpected(ready.error());
  }
  Result<wil::com_ptr_nothrow<ISearchCrawlScopeManager>> scope = crawl_scope();
  if (!scope) {
    return tl::unexpected(scope.error());
  }
  return work(**scope);
}

}  // namespace

Result<std::vector<indexer::ScopeRule>> Win32IndexerScope::rules() const {
  return with_scope<std::vector<indexer::ScopeRule>>(
      [](ISearchCrawlScopeManager& scope) -> Result<std::vector<indexer::ScopeRule>> {
        wil::com_ptr_nothrow<IEnumSearchScopeRules> list;
        if (const HRESULT got = scope.EnumerateScopeRules(&list); FAILED(got)) {
          return com_error("ISearchCrawlScopeManager::EnumerateScopeRules", got);
        }
        std::vector<indexer::ScopeRule> rules;
        for (;;) {
          wil::com_ptr_nothrow<ISearchScopeRule> rule;
          ULONG fetched = 0;
          const HRESULT next = list->Next(1, &rule, &fetched);
          if (FAILED(next)) {
            return com_error("IEnumSearchScopeRules::Next", next);
          }
          if (next != S_OK || fetched == 0) {
            return rules;
          }
          wil::unique_cotaskmem_string url;
          BOOL included = FALSE;
          BOOL is_default = FALSE;
          if (const HRESULT read = rule->get_PatternOrURL(&url); FAILED(read)) {
            return com_error("ISearchScopeRule::get_PatternOrURL", read);
          }
          if (const HRESULT read = rule->get_IsIncluded(&included); FAILED(read)) {
            return com_error("ISearchScopeRule::get_IsIncluded", read);
          }
          if (const HRESULT read = rule->get_IsDefault(&is_default); FAILED(read)) {
            return com_error("ISearchScopeRule::get_IsDefault", read);
          }
          rules.push_back(indexer::ScopeRule{.url = url ? std::wstring(url.get()) : std::wstring{},
                                             .included = included != FALSE,
                                             .user = is_default == FALSE});
        }
      });
}

Result<bool> Win32IndexerScope::included(const std::filesystem::path& folder) const {
  const std::wstring url = indexer::folder_url(folder);
  return with_scope<bool>([&url](ISearchCrawlScopeManager& scope) -> Result<bool> {
    BOOL included = FALSE;
    if (const HRESULT got = scope.IncludedInCrawlScope(url.c_str(), &included); FAILED(got)) {
      return com_error("ISearchCrawlScopeManager::IncludedInCrawlScope", got);
    }
    return included != FALSE;
  });
}

Result<void> Win32IndexerScope::set_rule(const std::filesystem::path& folder, bool include) {
  const std::wstring url = indexer::folder_url(folder);
  return with_scope<void>([&](ISearchCrawlScopeManager& scope) -> Result<void> {
    // fOverrideChildren FALSE: the rules of folders inside it stay, so undo restores the scope.
    if (const HRESULT added =
            scope.AddUserScopeRule(url.c_str(), include ? TRUE : FALSE, FALSE, FF_INDEXCOMPLEXURLS);
        FAILED(added)) {
      return com_error("ISearchCrawlScopeManager::AddUserScopeRule", added);
    }
    if (const HRESULT saved = scope.SaveAll(); FAILED(saved)) {
      return com_error("ISearchCrawlScopeManager::SaveAll", saved);
    }
    return {};
  });
}

Result<void> Win32IndexerScope::remove_rule(const std::filesystem::path& folder) {
  const std::wstring url = indexer::folder_url(folder);
  return with_scope<void>([&url](ISearchCrawlScopeManager& scope) -> Result<void> {
    const HRESULT removed = scope.RemoveScopeRule(url.c_str());
    if (FAILED(removed)) {
      return com_error("ISearchCrawlScopeManager::RemoveScopeRule", removed);
    }
    if (removed == S_FALSE) {
      return fail(Errc::not_found, "no index rule for " + utf8(url));
    }
    if (const HRESULT saved = scope.SaveAll(); FAILED(saved)) {
      return com_error("ISearchCrawlScopeManager::SaveAll", saved);
    }
    return {};
  });
}

}  // namespace myclean::platform
