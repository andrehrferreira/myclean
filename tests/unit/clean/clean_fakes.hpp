#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

// In-memory probes for the cleaning engine. Paths compare ASCII case-insensitively.
namespace myclean::clean::testing {

inline std::wstring key_of(const std::filesystem::path& path) {
  std::wstring key = path.native();
  for (wchar_t& character : key) {
    if (character >= L'A' && character <= L'Z') {
      character = static_cast<wchar_t>(character - L'A' + L'a');
    }
  }
  return key;
}

// A tree of entries with times. Missing parent folders are added with a zero time.
class FakeTree final : public IFileTree {
 public:
  void add_directory(const std::filesystem::path& path, WallTime touched = {},
                     os::FileAttributes attributes = {}) {
    add(path, os::EntryKind::directory, Bytes{}, touched, attributes);
  }
  void add_file(const std::filesystem::path& path, Bytes size, WallTime touched,
                os::FileAttributes attributes = {}) {
    add(path, os::EntryKind::file, attributes.cloud_placeholder() ? Bytes{} : size, touched,
        attributes);
  }
  // Listing `path` fails with access_denied from now on.
  void deny(const std::filesystem::path& path) { denied_.insert(key_of(path)); }
  // Number of list() calls of `path` so far.
  [[nodiscard]] int listed(const std::filesystem::path& path) const {
    const auto found = list_calls_.find(key_of(path));
    return found == list_calls_.end() ? 0 : found->second;
  }

  [[nodiscard]] Result<Entry> stat(const std::filesystem::path& path) const override {
    const auto found = entries_.find(key_of(path));
    if (found == entries_.end()) {
      return fail(Errc::not_found, "no such entry");
    }
    return found->second;
  }

  [[nodiscard]] Result<std::vector<Entry>> list(const std::filesystem::path& path) const override {
    ++list_calls_[key_of(path)];
    const auto self = entries_.find(key_of(path));
    if (self == entries_.end()) {
      return fail(Errc::not_found, "no such folder");
    }
    if (self->second.stat.kind != os::EntryKind::directory ||
        self->second.stat.attributes.reparse_point) {
      return fail(Errc::invalid_argument, "not a folder");
    }
    if (denied_.contains(self->first)) {
      return fail(Errc::access_denied, "denied");
    }
    std::vector<Entry> children;
    for (const auto& [key, entry] : entries_) {
      if (entry.path.has_relative_path() && key_of(entry.path.parent_path()) == self->first) {
        children.push_back(entry);
      }
    }
    return children;
  }

  // Every volume root in the tree, unless set_fixed_volumes() named them.
  void set_fixed_volumes(std::vector<std::filesystem::path> volumes) {
    fixed_volumes_ = std::move(volumes);
  }

  [[nodiscard]] std::vector<std::filesystem::path> fixed_volumes() const override {
    if (fixed_volumes_) {
      return *fixed_volumes_;
    }
    std::vector<std::filesystem::path> roots;
    for (const auto& [key, entry] : entries_) {
      if (!entry.path.has_relative_path()) {
        roots.push_back(entry.path);
      }
    }
    return roots;
  }

 private:
  void add(const std::filesystem::path& path, os::EntryKind kind, Bytes size, WallTime touched,
           os::FileAttributes attributes) {
    std::vector<std::filesystem::path> missing;  // parents, nearest first
    for (std::filesystem::path parent = path;
         parent.has_relative_path() && !entries_.contains(key_of(parent.parent_path()));) {
      parent = parent.parent_path();
      missing.push_back(parent);
    }
    for (const std::filesystem::path& parent : std::views::reverse(missing)) {
      insert(parent, os::EntryKind::directory, Bytes{}, WallTime{}, {});
    }
    insert(path, kind, size, touched, attributes);
  }

  void insert(const std::filesystem::path& path, os::EntryKind kind, Bytes size, WallTime touched,
              os::FileAttributes attributes) {
    if (entries_.contains(key_of(path))) {
      throw std::logic_error("FakeTree: entry exists");
    }
    Entry entry{
        .path = path,
        .stat = {.kind = kind, .attributes = attributes, .size = size, .size_on_disk = size},
        .touched = touched};
    entry.stat.identity.volume_serial = path.root_name().native().front();
    entry.stat.identity.file_id[0] = static_cast<std::uint8_t>(++next_id_);
    entries_.emplace(key_of(path), std::move(entry));
  }

  std::map<std::wstring, Entry> entries_;
  std::set<std::wstring> denied_;
  std::optional<std::vector<std::filesystem::path>> fixed_volumes_;
  mutable std::map<std::wstring, int> list_calls_;
  int next_id_ = 0;
};

class FakeEnvironment final : public IEnvironment {
 public:
  void set(std::wstring_view name, std::wstring value) {
    values_[key_of(std::filesystem::path{name})] = std::move(value);
  }
  // A variable of several values (STEAM_LIBRARY); variable() does not see it.
  void set_values(std::wstring_view name, std::vector<std::wstring> values) {
    lists_[key_of(std::filesystem::path{name})] = std::move(values);
  }

  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view name) const override {
    const auto found = values_.find(key_of(std::filesystem::path{name}));
    return found == values_.end() ? std::nullopt : std::optional{found->second};
  }
  [[nodiscard]] std::vector<std::wstring> values(std::wstring_view name) const override {
    const auto found = lists_.find(key_of(std::filesystem::path{name}));
    return found == lists_.end() ? IEnvironment::values(name) : found->second;
  }

 private:
  std::map<std::wstring, std::wstring> values_;
  std::map<std::wstring, std::vector<std::wstring>> lists_;
};

class FakeUsage final : public IUsage {
 public:
  void lock(const std::filesystem::path& file) { locked_.insert(key_of(file)); }
  void add_process_folder(std::filesystem::path folder) { folders_.push_back(std::move(folder)); }

  [[nodiscard]] bool locked(const std::filesystem::path& file) const override {
    return locked_.contains(key_of(file));
  }
  [[nodiscard]] std::vector<std::filesystem::path> process_folders() const override {
    return folders_;
  }

 private:
  std::set<std::wstring> locked_;
  std::vector<std::filesystem::path> folders_;
};

// Answers from sets of paths; a work tree with an error set fails every question.
class FakeGit final : public IGit {
 public:
  void set_last_commit(const std::filesystem::path& work_tree, std::optional<WallTime> time) {
    commits_[key_of(work_tree)] = time;
  }
  void ignore(const std::filesystem::path& entry) { ignored_.insert(key_of(entry)); }
  void track(const std::filesystem::path& entry) { tracked_.insert(key_of(entry)); }
  void fail_in(const std::filesystem::path& work_tree) { failing_.insert(key_of(work_tree)); }

  [[nodiscard]] Result<std::optional<WallTime>> last_commit(
      const std::filesystem::path& work_tree) const override {
    if (failing_.contains(key_of(work_tree))) {
      return fail(Errc::io_error, "git failed");
    }
    const auto found = commits_.find(key_of(work_tree));
    return found == commits_.end() ? std::nullopt : found->second;
  }
  [[nodiscard]] Result<bool> ignored(const std::filesystem::path& work_tree,
                                     const std::filesystem::path& entry) const override {
    if (failing_.contains(key_of(work_tree))) {
      return fail(Errc::io_error, "git failed");
    }
    return ignored_.contains(key_of(entry));
  }
  [[nodiscard]] Result<bool> tracks_files(const std::filesystem::path& work_tree,
                                          const std::filesystem::path& entry) const override {
    if (failing_.contains(key_of(work_tree))) {
      return fail(Errc::io_error, "git failed");
    }
    return tracked_.contains(key_of(entry));
  }

 private:
  std::map<std::wstring, std::optional<WallTime>> commits_;
  std::set<std::wstring> ignored_;
  std::set<std::wstring> tracked_;
  std::set<std::wstring> failing_;
};

}  // namespace myclean::clean::testing
