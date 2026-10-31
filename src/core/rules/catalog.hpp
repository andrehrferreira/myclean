#pragma once

#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"

// The loaded data in effect. Each file is accepted or rejected whole: a rejected file keeps its
// last valid version in effect and is never partly applied (store spec, "Schema-Validated Data
// Loading"). Catalogs are not synchronized; reload and read from one thread, or guard them.
namespace myclean::rules {

// The entries of every file with one extension in one directory: data/rules/*.toml or
// data/kb/*.json.
template <typename Entry>
class FileSetCatalog {
 public:
  using Parser = Result<std::vector<Entry>> (*)(std::string_view text,
                                                const std::filesystem::path& file);

  FileSetCatalog(std::filesystem::path dir, std::filesystem::path extension, Parser parse);

  // Re-reads every file. A valid file replaces its previous version. A file that cannot be read,
  // fails its schema or repeats an id of a file earlier by name adds one error and keeps its
  // last valid version (none if it never had one). A file that is gone drops its entries. When
  // `dir` cannot be listed, nothing changes and that is the only error.
  [[nodiscard]] std::vector<Error> reload(const IDataFiles& files);

  // Ordered by file name, then by position in the file.
  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
  // Null when no entry has `id`.
  [[nodiscard]] const Entry* find(std::string_view id) const;

 private:
  std::filesystem::path dir_;
  std::filesystem::path extension_;
  Parser parse_;
  std::map<std::filesystem::path, std::vector<Entry>> accepted_;  // per file, in effect
  std::vector<Entry> entries_;
};

// Instantiated in catalog.cpp for these two entry types.
extern template class FileSetCatalog<CleaningRule>;
extern template class FileSetCatalog<KbEntry>;

using RuleCatalog = FileSetCatalog<CleaningRule>;
using KnowledgeBase = FileSetCatalog<KbEntry>;

// <data_dir>/rules/*.toml
[[nodiscard]] RuleCatalog make_rule_catalog(const std::filesystem::path& data_dir);
// <data_dir>/kb/*.json
[[nodiscard]] KnowledgeBase make_knowledge_base(const std::filesystem::path& data_dir);

// The governor's classes from one file: data/classes.toml.
class ClassCatalog {
 public:
  explicit ClassCatalog(std::filesystem::path file);

  // Re-reads the file. On any failure the previous class set stays in effect.
  [[nodiscard]] Result<void> reload(const IDataFiles& files);

  // Empty (every process `default`, default caps) until the first successful reload.
  [[nodiscard]] const ClassSet& classes() const { return classes_; }

 private:
  std::filesystem::path file_;
  ClassSet classes_;
};

// <data_dir>/classes.toml
[[nodiscard]] ClassCatalog make_class_catalog(const std::filesystem::path& data_dir);

}  // namespace myclean::rules
