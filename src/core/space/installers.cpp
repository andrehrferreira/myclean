#include "core/space/installers.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::space {

namespace {

// Words that do not tell products or vendors apart.
constexpr std::array<std::string_view, 25> ignored_words = {
    "inc",   "ltd",     "llc",   "corp",  "corporation", "co",      "company", "gmbh", "ag",
    "sa",    "limited", "the",   "setup", "installer",   "install", "x64",     "x86",  "amd64",
    "arm64", "win64",   "win32", "bit",   "edition",     "version", "for"};

bool is_alphanumeric(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// A version number such as "23", or "v2".
bool is_version(std::string_view word) {
  if (word.starts_with('v') && word.size() > 1) {
    word.remove_prefix(1);
  }
  return std::ranges::all_of(word, [](char c) { return c >= '0' && c <= '9'; });
}

// The significant words of a name, lower case, in order.
std::vector<std::string> words(std::string_view name) {
  const std::string lower = lower_ascii(name);
  std::vector<std::string> result;
  for (std::size_t start = 0; start < lower.size();) {
    std::size_t end = start;
    while (end < lower.size() && is_alphanumeric(lower[end])) {
      ++end;
    }
    if (end > start) {
      std::string word = lower.substr(start, end - start);
      if (!is_version(word) && std::ranges::find(ignored_words, word) == ignored_words.end()) {
        result.push_back(std::move(word));
      }
    }
    start = end + 1;
  }
  return result;
}

bool contains_run(const std::vector<std::string>& haystack,
                  const std::vector<std::string>& needle) {
  return !std::ranges::search(haystack, needle).empty();
}

bool starts_with(const std::vector<std::string>& text, const std::vector<std::string>& prefix) {
  return prefix.size() <= text.size() && std::equal(prefix.begin(), prefix.end(), text.begin());
}

}  // namespace

std::string_view to_string(InstallerKind kind) {
  return kind == InstallerKind::msi ? "msi" : "exe";
}

bool matches_program(const VersionInfo& version, const InstalledProgram& program) {
  const std::vector<std::string> product = words(version.product_name);
  const std::vector<std::string> display = words(program.display_name);
  const std::vector<std::string> company = words(version.company_name);
  const std::vector<std::string> publisher = words(program.publisher);
  if (product.empty() || display.empty() || company.empty() || publisher.empty()) {
    return false;
  }
  const bool same_product = contains_run(display, product) || contains_run(product, display);
  const bool same_vendor = starts_with(publisher, company) || starts_with(company, publisher);
  return same_product && same_vendor;
}

namespace {

// Fills in what the installer `used` installed; false when that is not installed.
Result<bool> identify(UsedInstaller& used, const IInstallerInspector& inspector,
                      const std::vector<InstalledProgram>& programs) {
  if (used.kind == InstallerKind::msi) {
    const Result<std::optional<std::string>> code = inspector.msi_product_code(used.path);
    if (!code || !*code) {
      return code ? Result<bool>{false} : tl::unexpected(code.error());
    }
    const Result<std::optional<std::string>> product = inspector.installed_msi_product(**code);
    if (!product || !*product) {
      return product ? Result<bool>{false} : tl::unexpected(product.error());
    }
    used.product_code = **code;
    used.installed_as = **product;
    return true;
  }
  const Result<std::optional<VersionInfo>> version = inspector.exe_version(used.path);
  if (!version || !*version) {
    return version ? Result<bool>{false} : tl::unexpected(version.error());
  }
  const auto match = std::ranges::find_if(programs, [&](const InstalledProgram& program) {
    return matches_program(**version, program);
  });
  if (match == programs.end()) {
    return false;
  }
  used.installed_as = match->display_name;
  used.publisher = match->publisher;
  return true;
}

}  // namespace

Result<InstallerReport> used_installers(const ScanResult& scan,
                                        const IInstallerInspector& inspector) {
  const Result<std::vector<InstalledProgram>> programs = inspector.installed_programs();
  if (!programs) {
    return tl::unexpected(programs.error());
  }

  InstallerReport report;
  for (const Folder& folder : scan.folders) {
    for (const FileItem& file : folder.files) {
      const std::string extension = lower_extension(file.name);
      if (file.link || file.extra_link || file.placeholder ||
          (extension != ".msi" && extension != ".exe")) {
        continue;
      }
      UsedInstaller used{.path = path_of(folder, file),
                         .kind = extension == ".msi" ? InstallerKind::msi : InstallerKind::exe,
                         .size = file.size,
                         .size_on_disk = file.size_on_disk};
      const Result<bool> installed = identify(used, inspector, *programs);
      if (!installed) {
        report.errors.push_back(ItemError{.path = used.path, .error = installed.error()});
      } else if (*installed) {
        report.used.push_back(std::move(used));
      }
    }
  }
  std::ranges::sort(report.used, [](const UsedInstaller& left, const UsedInstaller& right) {
    return left.size_on_disk != right.size_on_disk ? left.size_on_disk > right.size_on_disk
                                                   : left.path < right.path;
  });
  return report;
}
}  // namespace myclean::space
