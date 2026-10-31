#include "core/rules/catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/parse_support.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {

namespace {

using Owners = std::map<std::string, std::filesystem::path, std::less<>>;  // id -> file

// Records the ids of `entries` as defined by `file`, unless one is already defined by another
// file; then nothing is recorded and the error names both files.
template <typename Entry>
std::optional<Error> claim_ids(const std::vector<Entry>& entries, const std::filesystem::path& file,
                               Owners& owners) {
  for (const Entry& entry : entries) {
    if (const auto owner = owners.find(entry.id); owner != owners.end()) {
      return Error{.code = Errc::invalid_data,
                   .message = detail::utf8(file) + ": entry '" + entry.id +
                              "' repeats an id defined in " + detail::utf8(owner->second)};
    }
  }
  for (const Entry& entry : entries) {
    owners.emplace(entry.id, file);
  }
  return std::nullopt;
}

}  // namespace

template <typename Entry>
FileSetCatalog<Entry>::FileSetCatalog(std::filesystem::path dir, std::filesystem::path extension,
                                      Parser parse)
    : dir_(std::move(dir)), extension_(std::move(extension)), parse_(parse) {}

template <typename Entry>
std::vector<Error> FileSetCatalog<Entry>::reload(const IDataFiles& files) {
  Result<std::vector<std::filesystem::path>> listed = files.list(dir_);
  if (!listed) {
    return {listed.error()};
  }
  std::vector<std::filesystem::path> paths;
  std::ranges::copy_if(
      *listed, std::back_inserter(paths),
      [this](const std::filesystem::path& path) { return path.extension() == extension_; });
  std::ranges::sort(paths);

  std::vector<Error> errors;
  std::map<std::filesystem::path, std::vector<Entry>> accepted;
  Owners owners;
  for (const std::filesystem::path& path : paths) {
    Result<std::vector<Entry>> parsed =
        files.read(path).and_then([&](const std::string& text) { return parse_(text, path); });
    if (parsed) {
      std::optional<Error> clash = claim_ids(*parsed, path, owners);
      if (!clash) {
        accepted.emplace(path, std::move(*parsed));
        continue;
      }
      errors.push_back(std::move(*clash));
    } else {
      errors.push_back(std::move(parsed.error()));
    }
    // Rejected: the last valid version of this file stays in effect.
    if (const auto previous = accepted_.find(path);
        previous != accepted_.end() && !claim_ids(previous->second, path, owners)) {
      accepted.emplace(path, previous->second);
    }
  }

  accepted_ = std::move(accepted);
  entries_.clear();
  for (const auto& [path, entries] : accepted_) {
    entries_.insert(entries_.end(), entries.begin(), entries.end());
  }
  return errors;
}

template <typename Entry>
const Entry* FileSetCatalog<Entry>::find(std::string_view id) const {
  const auto entry =
      std::ranges::find_if(entries_, [id](const Entry& candidate) { return candidate.id == id; });
  return entry == entries_.end() ? nullptr : &*entry;
}

template class FileSetCatalog<CleaningRule>;
template class FileSetCatalog<KbEntry>;

RuleCatalog make_rule_catalog(const std::filesystem::path& data_dir) {
  return {data_dir / "rules", ".toml", &parse_rule_file};
}

KnowledgeBase make_knowledge_base(const std::filesystem::path& data_dir) {
  return {data_dir / "kb", ".json", &parse_kb_file};
}

ClassCatalog::ClassCatalog(std::filesystem::path file) : file_(std::move(file)) {}

Result<void> ClassCatalog::reload(const IDataFiles& files) {
  Result<ClassSet> parsed = files.read(file_).and_then(
      [this](const std::string& text) { return parse_class_file(text, file_); });
  if (!parsed) {
    return tl::unexpected(std::move(parsed.error()));
  }
  classes_ = std::move(*parsed);
  return {};
}

ClassCatalog make_class_catalog(const std::filesystem::path& data_dir) {
  return ClassCatalog(data_dir / "classes.toml");
}

}  // namespace myclean::rules
