#include "fakes/fake_file_system.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

namespace myclean::fakes {

namespace {

namespace fs = std::filesystem;

// The path as Windows would resolve it lexically: "." and ".." applied, backslashes, and no
// trailing separator except on a root such as "C:\".
std::wstring spelled(const fs::path& path) {
  fs::path normal = path.lexically_normal().make_preferred();
  if (!normal.has_filename() && normal != normal.root_path()) {
    normal = normal.parent_path();
  }
  return normal.wstring();
}

std::wstring to_lower_ascii(std::wstring text) {
  for (wchar_t& character : text) {
    if (character >= L'A' && character <= L'Z') {
      character = static_cast<wchar_t>(character - L'A' + L'a');
    }
  }
  return text;
}

std::string narrow(const fs::path& path) {
  const std::u8string utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}

Result<void> require_absolute(const fs::path& path) {
  if (!path.is_absolute()) {
    return fail(Errc::invalid_argument, "path is not absolute: " + narrow(path));
  }
  return {};
}

}  // namespace

std::wstring FakeFileSystem::key_of(const fs::path& path) {
  return to_lower_ascii(spelled(path));
}

std::wstring FakeFileSystem::child_prefix(const std::wstring& key) {
  return key.ends_with(L'\\') ? key : key + L'\\';
}

std::uint64_t FakeFileSystem::volume_serial_of(const fs::path& path) {
  return std::hash<std::wstring>{}(to_lower_ascii(path.root_name().wstring()));
}

FileIdentity FakeFileSystem::next_identity(const fs::path& path) {
  FileIdentity identity{.volume_serial = volume_serial_of(path)};
  std::uint64_t value = next_file_id_++;
  for (std::size_t index = 0; index < 8; ++index) {
    identity.file_id.at(index) = static_cast<std::uint8_t>(value & 0xFFU);
    value >>= 8U;
  }
  return identity;
}

std::optional<Errc> FakeFileSystem::injected_failure(const fs::path& path,
                                                     OperationKind kind) const {
  const auto found = failures_.find(key_of(path));
  if (found == failures_.end() || (found->second.only && *found->second.only != kind)) {
    return std::nullopt;
  }
  return found->second.code;
}

std::vector<const FakeFileSystem::Node*> FakeFileSystem::children_of(
    const std::wstring& key) const {
  const std::wstring prefix = child_prefix(key);
  std::vector<const Node*> children;
  for (auto it = tree_.lower_bound(prefix); it != tree_.end() && it->first.starts_with(prefix);
       ++it) {
    if (it->first.find(L'\\', prefix.size()) == std::wstring::npos) {
      children.push_back(&it->second);
    }
  }
  return children;
}

bool FakeFileSystem::has_children(const std::wstring& key) const {
  const std::wstring prefix = child_prefix(key);
  const auto it = tree_.lower_bound(prefix);
  return it != tree_.end() && it->first.starts_with(prefix);
}

Result<void> FakeFileSystem::check_parent(const fs::path& path) const {
  const fs::path parent = fs::path{spelled(path)}.parent_path();
  const auto found = tree_.find(key_of(parent));
  if (found == tree_.end() || found->second.stat.kind != os::EntryKind::directory ||
      found->second.stat.attributes.reparse_point) {
    return fail(Errc::not_found, "parent is not a directory: " + narrow(parent));
  }
  return {};
}

void FakeFileSystem::ensure_parents(const fs::path& path) {
  const fs::path parent = fs::path{spelled(path)}.parent_path();
  const auto ensure_directory = [this](const fs::path& directory) {
    const auto found = tree_.find(key_of(directory));
    if (found == tree_.end()) {
      insert(directory, os::FileStat{.kind = os::EntryKind::directory});
    } else if (found->second.stat.kind != os::EntryKind::directory ||
               found->second.stat.attributes.reparse_point) {
      throw std::logic_error("fake: cannot create entries inside " + narrow(directory));
    }
  };
  fs::path current = parent.root_path();
  ensure_directory(current);
  for (const fs::path& component : parent.relative_path()) {
    current /= component;
    ensure_directory(current);
  }
}

FileIdentity FakeFileSystem::insert(const fs::path& path, os::FileStat stat) {
  stat.identity = next_identity(path);
  const FileIdentity identity = stat.identity;
  tree_.emplace(key_of(path), Node{.path = fs::path{spelled(path)}, .stat = stat});
  return identity;
}

FileIdentity FakeFileSystem::add_directory(const fs::path& path, os::FileAttributes attributes) {
  if (!path.is_absolute() || exists(path)) {
    throw std::logic_error("fake: cannot add directory " + narrow(path));
  }
  if (path.has_relative_path()) {
    ensure_parents(path);
  }
  return insert(path, os::FileStat{.kind = os::EntryKind::directory, .attributes = attributes});
}

FileIdentity FakeFileSystem::add_file(const fs::path& path, Bytes size,
                                      os::FileAttributes attributes) {
  if (!path.is_absolute() || !path.has_relative_path() || exists(path)) {
    throw std::logic_error("fake: cannot add file " + narrow(path));
  }
  ensure_parents(path);
  return insert(path,
                os::FileStat{.kind = os::EntryKind::file,
                             .attributes = attributes,
                             .size = size,
                             .size_on_disk = attributes.cloud_placeholder() ? Bytes{} : size});
}

void FakeFileSystem::set_short_name(const fs::path& path, const std::wstring& short_name) {
  const auto found = tree_.find(key_of(path));
  if (found == tree_.end()) {
    throw std::logic_error("fake: no entry " + narrow(path));
  }
  found->second.short_name = short_name;
}

void FakeFileSystem::fail_on(const fs::path& path, Errc code, std::optional<OperationKind> only) {
  failures_[key_of(path)] = Failure{.code = code, .only = only};
}

void FakeFileSystem::erase(const fs::path& path) {
  const std::wstring key = key_of(path);
  if (tree_.erase(key) == 0) {
    throw std::logic_error("fake: no entry " + narrow(path));
  }
  const std::wstring prefix = child_prefix(key);
  auto it = tree_.lower_bound(prefix);
  while (it != tree_.end() && it->first.starts_with(prefix)) {
    it = tree_.erase(it);
  }
}

bool FakeFileSystem::exists(const fs::path& path) const {
  return tree_.contains(key_of(path));
}

Result<os::FileStat> FakeFileSystem::stat(const fs::path& path) const {
  if (auto absolute = require_absolute(path); !absolute) {
    return tl::unexpected(absolute.error());
  }
  const auto found = tree_.find(key_of(path));
  if (found == tree_.end()) {
    return fail(Errc::not_found, "no such entry: " + narrow(path));
  }
  return found->second.stat;
}

Result<std::vector<os::DirEntry>> FakeFileSystem::enumerate(const fs::path& path) const {
  auto status = stat(path);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (status->kind != os::EntryKind::directory) {
    return fail(Errc::invalid_argument, "not a directory: " + narrow(path));
  }
  if (status->attributes.reparse_point) {
    return fail(Errc::invalid_argument, "is a reparse point: " + narrow(path));
  }
  std::vector<os::DirEntry> entries;
  for (const Node* child : children_of(key_of(path))) {
    entries.push_back(os::DirEntry{.path = child->path, .stat = child->stat});
  }
  return entries;
}

Result<fs::path> FakeFileSystem::expand_short_names(const fs::path& path) const {
  if (auto absolute = require_absolute(path); !absolute) {
    return tl::unexpected(absolute.error());
  }
  const fs::path target{spelled(path)};
  fs::path current = target.root_path();
  if (!exists(current)) {
    return fail(Errc::not_found, "no such volume: " + narrow(current));
  }
  for (const fs::path& component : target.relative_path()) {
    const std::wstring wanted = to_lower_ascii(component.wstring());
    const Node* match = nullptr;
    for (const Node* child : children_of(key_of(current))) {
      if (to_lower_ascii(child->path.filename().wstring()) == wanted ||
          (!child->short_name.empty() && to_lower_ascii(child->short_name) == wanted)) {
        match = child;
        break;
      }
    }
    if (match == nullptr) {
      return fail(Errc::not_found, "no such entry: " + narrow(current / component));
    }
    current = match->path;
  }
  return current;
}

Result<void> FakeFileSystem::remove(const fs::path& path) {
  return remove_entry(path, nullptr);
}

Result<void> FakeFileSystem::remove(const fs::path& path, const FileIdentity& expected) {
  return remove_entry(path, &expected);
}

Result<void> FakeFileSystem::remove_entry(const fs::path& path, const FileIdentity* expected) {
  if (on_remove_) {
    on_remove_(path);
  }
  operations_.push_back({.kind = OperationKind::remove, .from = path});
  if (auto code = injected_failure(path, OperationKind::remove)) {
    return fail(*code, "injected failure: " + narrow(path));
  }
  auto status = stat(path);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (expected != nullptr && status->identity != *expected) {
    return fail(Errc::refused, "entry changed identity: " + narrow(path));
  }
  const std::wstring key = key_of(path);
  if (status->kind == os::EntryKind::directory && has_children(key)) {
    return fail(Errc::not_empty, "directory not empty: " + narrow(path));
  }
  tree_.erase(key);
  return {};
}

Result<void> FakeFileSystem::rename(const fs::path& from, const fs::path& to) {
  operations_.push_back({.kind = OperationKind::rename, .from = from, .to = to});
  for (const fs::path* path : {&from, &to}) {
    if (auto code = injected_failure(*path, OperationKind::rename)) {
      return fail(*code, "injected failure: " + narrow(*path));
    }
    if (auto absolute = require_absolute(*path); !absolute) {
      return absolute;
    }
  }
  const std::wstring from_key = key_of(from);
  const std::wstring to_key = key_of(to);
  if (!tree_.contains(from_key)) {
    return fail(Errc::not_found, "no such entry: " + narrow(from));
  }
  if (tree_.contains(to_key)) {
    return fail(Errc::already_exists, "destination exists: " + narrow(to));
  }
  if (volume_serial_of(from) != volume_serial_of(to)) {
    return fail(Errc::invalid_argument, "rename across volumes: " + narrow(from));
  }
  if (to_key.starts_with(child_prefix(from_key))) {
    return fail(Errc::invalid_argument, "destination is inside the source: " + narrow(to));
  }
  if (auto parent = check_parent(to); !parent) {
    return parent;
  }

  const std::wstring new_spelling = spelled(to);
  const std::size_t old_length = from_key.size();
  const std::wstring prefix = child_prefix(from_key);
  std::vector<Node> moved;
  for (auto it = tree_.begin(); it != tree_.end();) {
    if (it->first == from_key || it->first.starts_with(prefix)) {
      Node node = std::move(it->second);
      node.path = fs::path{new_spelling + node.path.wstring().substr(old_length)};
      moved.push_back(std::move(node));
      it = tree_.erase(it);
    } else {
      ++it;
    }
  }
  for (Node& node : moved) {
    std::wstring key = key_of(node.path);
    tree_.emplace(std::move(key), std::move(node));
  }
  return {};
}

Result<void> FakeFileSystem::move_file(const fs::path& from, const fs::path& to) {
  operations_.push_back({.kind = OperationKind::move_file, .from = from, .to = to});
  for (const fs::path* path : {&from, &to}) {
    if (auto code = injected_failure(*path, OperationKind::move_file)) {
      return fail(*code, "injected failure: " + narrow(*path));
    }
    if (auto absolute = require_absolute(*path); !absolute) {
      return absolute;
    }
  }
  const auto found = tree_.find(key_of(from));
  if (found == tree_.end()) {
    return fail(Errc::not_found, "no such entry: " + narrow(from));
  }
  if (found->second.stat.kind != os::EntryKind::file ||
      found->second.stat.attributes.reparse_point) {
    return fail(Errc::invalid_argument, "not a regular file: " + narrow(from));
  }
  if (exists(to)) {
    return fail(Errc::already_exists, "destination exists: " + narrow(to));
  }
  if (auto parent = check_parent(to); !parent) {
    return parent;
  }
  Node node = std::move(found->second);
  tree_.erase(found);
  node.path = fs::path{spelled(to)};
  if (volume_serial_of(from) != volume_serial_of(to)) {
    node.stat.identity = next_identity(to);  // a copy on another volume is a new file
  }
  tree_.emplace(key_of(to), std::move(node));
  return {};
}

Result<os::FileStat> FakeFileSystem::copy_file(const fs::path& from, const fs::path& to,
                                               const FileIdentity& expected) {
  operations_.push_back({.kind = OperationKind::copy_file, .from = from, .to = to});
  for (const fs::path* path : {&from, &to}) {
    if (auto code = injected_failure(*path, OperationKind::copy_file)) {
      return fail(*code, "injected failure: " + narrow(*path));
    }
    if (auto absolute = require_absolute(*path); !absolute) {
      return tl::unexpected(absolute.error());
    }
  }
  const auto found = tree_.find(key_of(from));
  if (found == tree_.end()) {
    return fail(Errc::not_found, "no such entry: " + narrow(from));
  }
  const os::FileStat source = found->second.stat;
  if (source.identity != expected) {
    return fail(Errc::refused, "entry changed identity: " + narrow(from));
  }
  if (source.kind != os::EntryKind::file || source.attributes.reparse_point ||
      source.attributes.cloud_placeholder()) {
    return fail(Errc::invalid_argument, "not a regular local file: " + narrow(from));
  }
  if (exists(to)) {
    return fail(Errc::already_exists, "destination exists: " + narrow(to));
  }
  if (auto parent = check_parent(to); !parent) {
    return tl::unexpected(parent.error());
  }
  insert(to, source);
  return tree_.at(key_of(to)).stat;
}

Result<void> FakeFileSystem::flush(std::span<const fs::path> files) {
  if (files.empty()) {
    return {};
  }
  operations_.push_back({.kind = OperationKind::flush, .from = files.front()});
  for (const fs::path& file : files) {
    if (auto code = injected_failure(file, OperationKind::flush)) {
      return fail(*code, "injected failure: " + narrow(file));
    }
    if (!exists(file)) {
      return fail(Errc::not_found, "no such entry: " + narrow(file));
    }
  }
  return {};
}

Result<void> FakeFileSystem::create_directories(const fs::path& path) {
  operations_.push_back({.kind = OperationKind::create_directories, .from = path});
  if (auto code = injected_failure(path, OperationKind::create_directories)) {
    return fail(*code, "injected failure: " + narrow(path));
  }
  if (auto absolute = require_absolute(path); !absolute) {
    return absolute;
  }
  const fs::path target{spelled(path)};
  fs::path current = target.root_path();
  if (!exists(current)) {
    return fail(Errc::not_found, "no such volume: " + narrow(current));
  }
  for (const fs::path& component : target.relative_path()) {
    current /= component;
    const auto found = tree_.find(key_of(current));
    if (found == tree_.end()) {
      insert(current, os::FileStat{.kind = os::EntryKind::directory});
    } else if (found->second.stat.kind != os::EntryKind::directory ||
               found->second.stat.attributes.reparse_point) {
      return fail(Errc::invalid_argument, "not a directory or a reparse point: " + narrow(current));
    }
  }
  return {};
}

}  // namespace myclean::fakes
