#include "core/backup/repos.hpp"

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/space/scanner.hpp"

namespace myclean::backup {

namespace {

std::wstring lower(std::wstring text) {
  std::ranges::transform(text, text.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
  });
  return text;
}

bool skipped(const std::filesystem::path& name, std::span<const std::wstring> skip) {
  const std::wstring folded = lower(name.native());
  return std::ranges::any_of(skip,
                             [&](const std::wstring& entry) { return lower(entry) == folded; });
}

struct Pending {
  std::filesystem::path folder;
  int depth = 0;
};

// The work trees below `root`; sets `truncated` when it stopped at the folder limit.
void walk(const space::IDirectoryReader& reader, const std::filesystem::path& root,
          std::span<const std::wstring> skip, const RepoLimits& limits, const std::stop_token& stop,
          RepoWalk& found) {
  std::deque<Pending> pending{Pending{.folder = root, .depth = 0}};
  std::size_t listed = 0;
  while (!pending.empty() && !stop.stop_requested()) {
    if (listed == limits.max_folders) {
      found.truncated = true;
      return;
    }
    const Pending next = std::move(pending.front());
    pending.pop_front();
    Result<std::vector<space::ListedEntry>> entries = reader.list(next.folder);
    ++listed;
    if (!entries) {
      continue;
    }
    const bool repository = std::ranges::any_of(*entries, [](const space::ListedEntry& entry) {
      return lower(entry.name.native()) == L".git" && !entry.attributes.reparse_point;
    });
    if (repository) {
      found.repositories.push_back(next.folder);
      continue;
    }
    if (next.depth >= limits.max_depth) {
      continue;
    }
    for (const space::ListedEntry& entry : *entries) {
      if (entry.directory && !entry.attributes.reparse_point && !skipped(entry.name, skip)) {
        pending.push_back(Pending{.folder = next.folder / entry.name, .depth = next.depth + 1});
      }
    }
  }
}

std::string finding_text(const RepoFinding& finding) {
  std::vector<std::string> parts;
  if (finding.no_remote) {
    parts.emplace_back("no remote");
  }
  if (finding.unpushed > 0) {
    parts.push_back(std::to_string(finding.unpushed) + " unpushed commit" +
                    (finding.unpushed == 1 ? "" : "s"));
  }
  if (finding.uncommitted) {
    parts.emplace_back("uncommitted changes");
  }
  std::string text;
  for (const std::string& part : parts) {
    text += (text.empty() ? "" : "; ") + part;
  }
  return text;
}

}  // namespace

RepoWalk find_repositories(const space::IDirectoryReader& reader,
                           std::span<const std::filesystem::path> roots,
                           std::span<const std::wstring> skip, const RepoLimits& limits,
                           const std::stop_token& stop) {
  RepoWalk found;
  for (const std::filesystem::path& root : roots) {
    walk(reader, root, skip, limits, stop, found);
  }
  std::ranges::sort(found.repositories);
  return found;
}

RepoReport check_repositories(const IRepoGit& git,
                              std::span<const std::filesystem::path> repositories,
                              const std::stop_token& stop) {
  RepoReport report;
  for (const std::filesystem::path& repository : repositories) {
    if (stop.stop_requested()) {
      break;
    }
    const Result<RepoState> state = git.state(repository);
    if (!state) {
      report.problems.push_back(utf8_text(repository) + ": " + state.error().message);
      continue;
    }
    ++report.checked;
    RepoFinding finding{.path = repository,
                        .no_remote = state->remotes == 0,
                        // Without a remote every commit is unpushed; "no remote" says it.
                        .unpushed = state->remotes == 0 ? 0 : state->unpushed,
                        .uncommitted = state->uncommitted};
    if (finding.no_remote || finding.unpushed > 0 || finding.uncommitted) {
      finding.text = finding_text(finding);
      report.unprotected.push_back(std::move(finding));
    }
  }
  return report;
}

}  // namespace myclean::backup
