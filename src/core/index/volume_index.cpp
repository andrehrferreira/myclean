#include "core/index/volume_index.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/index/mft.hpp"
#include "core/index/usn.hpp"
#include "core/index/volume.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::index {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t no_node = std::numeric_limits<std::uint32_t>::max();

constexpr std::uint16_t flag_folder = 0x01;
constexpr std::uint16_t flag_link = 0x02;
constexpr std::uint16_t flag_placeholder = 0x04;
constexpr std::uint16_t flag_extra_link = 0x08;
constexpr std::uint16_t flag_dead = 0x10;

constexpr std::uint32_t placeholder_attributes = file_attribute::offline |
                                                 file_attribute::recall_on_open |
                                                 file_attribute::recall_on_data_access;

// Bounds every walk up a parent chain or along the links of a record, so that damaged data
// cannot loop forever.
constexpr std::size_t max_depth = 4096;
// How many missing parent folders apply() reads in a row before it gives a name up.
constexpr unsigned max_parent_reads = 256;

std::uint32_t unix_seconds(std::uint64_t file_time) {
  // FILETIME counts 100 ns ticks since 1601-01-01; the Unix epoch is 11644473600 s later.
  constexpr std::uint64_t epoch_ticks = 116444736000000000ULL;
  constexpr std::uint64_t ticks_per_second = 10'000'000;
  if (file_time <= epoch_ticks) {
    return 0;
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      (file_time - epoch_ticks) / ticks_per_second, std::numeric_limits<std::uint32_t>::max()));
}

WallTime wall_time(std::uint32_t seconds) {
  return WallTime{std::chrono::seconds{seconds}};
}

fs::path path_from_utf8(std::string_view text) {
  std::u8string utf8(text.size(), u8'\0');
  std::memcpy(utf8.data(), text.data(), text.size());
  return fs::path{utf8};
}

bool equal_ignoring_ascii_case(std::string_view left, std::string_view right) {
  return space::lower_ascii(left) == space::lower_ascii(right);
}

}  // namespace

VolumeIndex::VolumeIndex(fs::path root, std::uint64_t volume_serial)
    : root_(std::move(root)), serial_(volume_serial) {}

void VolumeIndex::fold(Facts& facts, const RecordSegment& segment) {
  facts.size_on_disk += segment.size_on_disk;
  if (segment.size) {
    facts.size = *segment.size;
  }
  if (segment.reparse_tag != 0) {
    facts.reparse_tag = segment.reparse_tag;
  }
  if (segment.base == 0) {
    facts.in_use = segment.in_use;
    facts.directory = segment.directory;
    facts.sequence = segment.sequence;
    if (segment.has_standard_information) {
      facts.attributes = segment.attributes;
      facts.created = unix_seconds(segment.created);
      facts.modified = unix_seconds(segment.modified);
    }
  }
}

std::uint16_t VolumeIndex::flags_of(const Facts& facts) {
  const bool reparse = (facts.attributes & file_attribute::reparse_point) != 0;
  if (reparse && (facts.directory || (facts.reparse_tag & name_surrogate_bit) != 0)) {
    return flag_link;
  }
  if (facts.directory) {
    return flag_folder;
  }
  return (facts.attributes & placeholder_attributes) != 0 ? flag_placeholder : 0;
}

std::optional<std::uint32_t> VolumeIndex::append_name(std::string_view name) {
  if (names_.size() > std::numeric_limits<std::uint32_t>::max() - name.size()) {
    return std::nullopt;
  }
  const auto offset = static_cast<std::uint32_t>(names_.size());
  names_ += name;
  return offset;
}

std::uint32_t VolumeIndex::append_node(std::uint32_t record, std::string_view name) {
  if (nodes_.size() >= no_node) {
    return no_node;
  }
  const std::optional<std::uint32_t> offset = append_name(name);
  if (!offset) {
    return no_node;
  }
  const auto node = static_cast<std::uint32_t>(nodes_.size());
  // A name is at most 255 UTF-16 units, so its UTF-8 form fits 16 bits.
  nodes_.push_back(Node{.parent = no_node,
                        .name_offset = *offset,
                        .record = record,
                        .next_link = no_node,
                        .name_length = static_cast<std::uint16_t>(name.size())});
  return node;
}

void VolumeIndex::set_facts(std::uint32_t node, const Facts& facts) {
  Node& target = nodes_[node];
  target.flags = flags_of(facts);
  target.sequence = facts.sequence;
  target.created = facts.created;
  target.modified = facts.modified;
  target.size = 0;
  target.size_on_disk = 0;
  if ((target.flags & flag_link) != 0) {
    target.size = facts.reparse_tag;  // a link has no size; its node keeps the tag there
  } else if ((target.flags & flag_folder) == 0) {
    target.size = facts.size;
    target.size_on_disk = facts.size_on_disk;
  }
}

std::string_view VolumeIndex::name_of(std::uint32_t node) const {
  return std::string_view{names_}.substr(nodes_[node].name_offset, nodes_[node].name_length);
}

fs::path VolumeIndex::path_of(std::uint32_t node) const {
  std::vector<std::uint32_t> chain;
  for (std::uint32_t at = node; at != root_node_ && at != no_node && chain.size() < max_depth;
       at = nodes_[at].parent) {
    chain.push_back(at);
  }
  std::ranges::reverse(chain);
  fs::path path = root_;
  for (const std::uint32_t at : chain) {
    path /= path_from_utf8(name_of(at));
  }
  return path;
}

bool VolumeIndex::live(std::uint32_t node) const {
  return (nodes_[node].flags & flag_dead) == 0;
}

bool VolumeIndex::is_folder(std::uint32_t node) const {
  return (nodes_[node].flags & (flag_folder | flag_dead)) == flag_folder;
}

bool VolumeIndex::is_link(std::uint32_t node) const {
  return (nodes_[node].flags & flag_link) != 0;
}

bool VolumeIndex::counted(std::uint32_t node) const {
  return (nodes_[node].flags & (flag_folder | flag_link | flag_extra_link | flag_dead)) == 0;
}

bool VolumeIndex::several_links(std::uint32_t node) const {
  return nodes_[node].next_link != no_node || first_node_[nodes_[node].record] != node;
}

std::vector<std::uint32_t> VolumeIndex::nodes_of(std::uint32_t record) const {
  std::vector<std::uint32_t> nodes;
  if (record >= first_node_.size()) {
    return nodes;
  }
  for (std::uint32_t node = first_node_[record]; node != no_node && nodes.size() < max_depth;
       node = nodes_[node].next_link) {
    nodes.push_back(node);
  }
  return nodes;
}

bool VolumeIndex::before(std::uint32_t a, std::uint32_t b) const {
  const auto chain = [this](std::uint32_t node) {
    std::vector<std::uint32_t> folders;
    for (std::uint32_t at = nodes_[node].parent; at != no_node && folders.size() < max_depth;
         at = nodes_[at].parent) {
      folders.push_back(at);
    }
    std::ranges::reverse(folders);
    return folders;
  };
  const std::vector<std::uint32_t> left = chain(a);
  const std::vector<std::uint32_t> right = chain(b);
  const auto [left_end, right_end] = std::ranges::mismatch(left, right);
  if (left_end == left.end() && right_end == right.end()) {
    return name_of(a) < name_of(b);
  }
  if (left_end == left.end()) {
    return true;  // the files of a folder come before its subfolders
  }
  if (right_end == right.end()) {
    return false;
  }
  return name_of(*left_end) < name_of(*right_end);
}

void VolumeIndex::map(std::uint32_t node) {
  const std::uint32_t record = nodes_[node].record;
  if (first_node_.size() <= record) {
    first_node_.resize(std::size_t{record} + 1, no_node);
  }
  nodes_[node].next_link = first_node_[record];
  first_node_[record] = node;
}

void VolumeIndex::unmap(std::uint32_t node) {
  const std::uint32_t record = nodes_[node].record;
  if (record >= first_node_.size()) {
    return;
  }
  if (first_node_[record] == node) {
    first_node_[record] = nodes_[node].next_link;
  } else {
    for (std::uint32_t at = first_node_[record]; at != no_node; at = nodes_[at].next_link) {
      if (nodes_[at].next_link == node) {
        nodes_[at].next_link = nodes_[node].next_link;
        break;
      }
    }
  }
  nodes_[node].next_link = no_node;
}

void VolumeIndex::choose_counted(std::uint32_t record) {
  const std::vector<std::uint32_t> nodes = nodes_of(record);
  if (nodes.empty()) {
    return;
  }
  std::uint32_t first = nodes.front();
  for (const std::uint32_t node : nodes) {
    if (before(node, first)) {
      first = node;
    }
  }
  for (const std::uint32_t node : nodes) {
    nodes_[node].flags =
        static_cast<std::uint16_t>(node == first ? nodes_[node].flags & ~flag_extra_link
                                                 : nodes_[node].flags | flag_extra_link);
  }
}

void VolumeIndex::credit(std::uint32_t node, bool add) {
  const std::uint64_t size = nodes_[node].size;
  const std::uint64_t size_on_disk = nodes_[node].size_on_disk;
  std::uint32_t at = nodes_[node].parent;
  for (std::size_t depth = 0; at != no_node && live(at) && depth < max_depth; ++depth) {
    Node& folder = nodes_[at];
    if (add) {
      folder.size += size;
      folder.size_on_disk += size_on_disk;
    } else {
      folder.size -= size;
      folder.size_on_disk -= size_on_disk;
    }
    at = folder.parent;
  }
}

void VolumeIndex::detach(std::uint32_t node) {
  if (!live(node)) {
    return;
  }
  if (is_folder(node) || counted(node)) {
    credit(node, false);
  }
  unmap(node);
  nodes_[node].flags |= flag_dead;
  ++dead_;
}

std::uint32_t VolumeIndex::folder_node(std::uint64_t reference) const {
  const std::uint64_t record = record_of(reference);
  if (record >= first_node_.size()) {
    return no_node;
  }
  const std::uint32_t node = first_node_[record];
  if (node == no_node || !is_folder(node) || nodes_[node].sequence != sequence_of(reference)) {
    return no_node;
  }
  return node;
}

void VolumeIndex::add(const RecordSegment& segment) {
  const std::uint64_t record = segment.base != 0 ? record_of(segment.base) : segment.record;
  if (record >= no_node) {
    return;  // NTFS numbers records in 32 bits
  }
  if (facts_.size() <= record) {
    facts_.resize(record + 1);
  }
  fold(facts_[record], segment);
  for (const FileName& name : segment.names) {
    if (append_node(static_cast<std::uint32_t>(record), name.name) == no_node) {
      ++unparented_;
      continue;
    }
    parent_refs_.push_back(name.parent);
  }
}

void VolumeIndex::link_parents() {
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    if (live(node) && node != root_node_) {
      nodes_[node].parent = folder_node(parent_refs_[node]);
    }
  }
  nodes_[root_node_].parent = no_node;

  // A name is kept when its parent chain reaches the root: 1 reachable, 2 not, 3 being walked.
  std::vector<std::uint8_t> state(nodes_.size(), 0);
  state[root_node_] = 1;
  std::vector<std::uint32_t> walk;
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    walk.clear();
    std::uint32_t at = node;
    while (at != no_node && live(at) && state[at] == 0) {
      state[at] = 3;
      walk.push_back(at);
      at = nodes_[at].parent;
    }
    const std::uint8_t verdict = at != no_node && live(at) && state[at] == 1 ? 1 : 2;
    for (const std::uint32_t walked : walk) {
      state[walked] = verdict;
    }
  }
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    if (live(node) && state[node] == 2) {
      unmap(node);
      nodes_[node].flags |= flag_dead;
      ++dead_;
      ++unparented_;
    }
  }
}

Result<void> VolumeIndex::finish() {
  if (facts_.size() <= root_record || !facts_[root_record].in_use ||
      !facts_[root_record].directory) {
    return fail(Errc::invalid_data, "the $MFT has no root folder");
  }
  first_node_.assign(facts_.size(), no_node);
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    const Facts& facts = facts_[nodes_[node].record];
    if (!facts.in_use) {
      nodes_[node].flags = flag_dead;  // a name left in an extension segment of a deleted file
      ++dead_;
      continue;
    }
    set_facts(node, facts);
    if (is_folder(node)) {
      if (first_node_[nodes_[node].record] == no_node) {
        map(node);
      } else {
        nodes_[node].flags |= flag_dead;  // a folder has one name
        ++dead_;
      }
    }
  }
  root_node_ = first_node_[root_record];
  if (root_node_ == no_node) {
    return fail(Errc::invalid_data, "the $MFT root folder has no name");
  }
  link_parents();
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    if (live(node) && !is_folder(node)) {
      map(node);
    }
  }
  for (std::uint32_t record = 0; record < first_node_.size(); ++record) {
    const std::uint32_t node = first_node_[record];
    if (node != no_node && nodes_[node].next_link != no_node) {
      choose_counted(record);
    }
  }
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    if (counted(node)) {
      credit(node, true);
    }
  }
  facts_ = {};
  parent_refs_ = {};
  nodes_.shrink_to_fit();
  names_.shrink_to_fit();
  return {};
}

Result<std::optional<VolumeIndex::FileState>> VolumeIndex::read_file(const INtfsVolume& volume,
                                                                     std::uint64_t record) {
  Result<std::optional<std::vector<std::byte>>> bytes = volume.file_record(record);
  if (!bytes) {
    return tl::unexpected(bytes.error());
  }
  std::optional<std::vector<std::byte>>& base = *bytes;
  if (!base) {
    return std::optional<FileState>{};
  }
  RecordSegment segment;
  if (Result<void> parsed = parse_record(*base, segment); !parsed) {
    return tl::unexpected(parsed.error());
  }
  if (!segment.in_use || segment.base != 0) {
    return std::optional<FileState>{};
  }
  FileState state;
  fold(state.facts, segment);
  state.names = std::move(segment.names);
  if (!segment.has_attribute_list) {
    return std::optional<FileState>{std::move(state)};
  }

  Result<std::vector<std::byte>> list =
      segment.attribute_list_extents.empty()
          ? Result<std::vector<std::byte>>{segment.attribute_list}
          : read_extents(volume, segment.attribute_list_extents, segment.attribute_list_length);
  if (!list) {
    return tl::unexpected(list.error());
  }
  const Result<std::vector<std::uint64_t>> extensions = extension_records(*list, record);
  if (!extensions) {
    return tl::unexpected(extensions.error());
  }
  for (const std::uint64_t extension : *extensions) {
    Result<std::optional<std::vector<std::byte>>> part_bytes = volume.file_record(extension);
    if (!part_bytes) {
      return tl::unexpected(part_bytes.error());
    }
    std::optional<std::vector<std::byte>>& found = *part_bytes;
    if (!found) {
      continue;
    }
    RecordSegment part;
    if (Result<void> parsed = parse_record(*found, part); !parsed) {
      return tl::unexpected(parsed.error());
    }
    if (part.in_use && record_of(part.base) == record) {
      fold(state.facts, part);
      state.names.insert(state.names.end(), part.names.begin(), part.names.end());
    }
  }
  return std::optional<FileState>{std::move(state)};
}

void VolumeIndex::remove(std::uint64_t reference) {
  const std::uint64_t record = record_of(reference);
  if (record >= first_node_.size()) {
    return;
  }
  for (const std::uint32_t node : nodes_of(static_cast<std::uint32_t>(record))) {
    if (nodes_[node].sequence == sequence_of(reference)) {
      detach(node);
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by max_parent_reads
Result<std::uint32_t> VolumeIndex::resolve_parent(std::uint64_t reference,
                                                  const INtfsVolume& volume, unsigned depth) {
  if (const std::uint32_t node = folder_node(reference); node != no_node) {
    return node;
  }
  if (depth >= max_parent_reads) {
    return no_node;
  }
  if (Result<void> refreshed = refresh(reference, volume, depth + 1); !refreshed) {
    return tl::unexpected(refreshed.error());
  }
  return folder_node(reference);
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by max_parent_reads
Result<void> VolumeIndex::refresh(std::uint64_t reference, const INtfsVolume& volume,
                                  unsigned depth) {
  const std::uint64_t record = record_of(reference);
  if (record >= no_node) {
    return {};
  }
  const Result<std::optional<FileState>> state = read_file(volume, record);
  if (!state) {
    return tl::unexpected(state.error());
  }
  const std::optional<FileState>& file = *state;
  if (!file || file->facts.sequence != sequence_of(reference)) {
    remove(reference);  // that file is gone; a newer one in the record has its own entries
    return {};
  }
  return upsert(static_cast<std::uint32_t>(record), *file, volume, depth);
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by max_parent_reads
Result<void> VolumeIndex::upsert(std::uint32_t record, const FileState& state,
                                 const INtfsVolume& volume, unsigned depth) {
  for (const std::uint32_t node : nodes_of(record)) {
    if (nodes_[node].sequence != state.facts.sequence) {
      detach(node);  // an older file that used the same record
    }
  }
  const std::vector<std::uint32_t> existing = nodes_of(record);
  if (flags_of(state.facts) == flag_folder && existing.size() == 1 && is_folder(existing.front())) {
    return move_folder(existing.front(), state, volume, depth);
  }
  for (const std::uint32_t node : existing) {
    detach(node);
  }
  return relink(record, state, volume, depth);
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by max_parent_reads
Result<void> VolumeIndex::move_folder(std::uint32_t node, const FileState& state,
                                      const INtfsVolume& volume, unsigned depth) {
  // A folder keeps its node, so its contents stay linked; its totals move with it.
  if (node == root_node_) {
    nodes_[node].modified = state.facts.modified;
    return {};
  }
  if (state.names.empty()) {
    detach(node);
    return {};
  }
  const FileName& name = state.names.front();
  const Result<std::uint32_t> parent = resolve_parent(name.parent, volume, depth);
  if (!parent) {
    return tl::unexpected(parent.error());
  }
  if (*parent == no_node) {
    detach(node);
    ++unparented_;
    return {};
  }
  credit(node, false);
  if (name_of(node) != name.name) {
    if (const std::optional<std::uint32_t> offset = append_name(name.name)) {
      nodes_[node].name_offset = *offset;
      nodes_[node].name_length = static_cast<std::uint16_t>(name.name.size());
    }
  }
  nodes_[node].parent = *parent;
  nodes_[node].created = state.facts.created;
  nodes_[node].modified = state.facts.modified;
  credit(node, true);
  return {};
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by max_parent_reads
Result<void> VolumeIndex::relink(std::uint32_t record, const FileState& state,
                                 const INtfsVolume& volume, unsigned depth) {
  const bool folder = flags_of(state.facts) == flag_folder;
  std::vector<std::uint32_t> added;
  for (const FileName& name : state.names) {
    if (folder && !added.empty()) {
      break;  // a folder has one name
    }
    const Result<std::uint32_t> parent = resolve_parent(name.parent, volume, depth);
    if (!parent) {
      return tl::unexpected(parent.error());
    }
    const std::uint32_t node = *parent == no_node ? no_node : append_node(record, name.name);
    if (node == no_node) {
      ++unparented_;
      continue;
    }
    set_facts(node, state.facts);
    nodes_[node].parent = *parent;
    map(node);
    added.push_back(node);
  }
  if (added.size() > 1 && !folder) {
    choose_counted(record);
  }
  for (const std::uint32_t node : added) {
    if (counted(node)) {
      credit(node, true);
    }
  }
  return {};
}

Result<void> VolumeIndex::apply(std::span<const UsnRecord> records, const INtfsVolume& volume) {
  // Only the last entry of each file matters: the file is re-read as it is now.
  std::unordered_map<std::uint64_t, std::size_t> last;
  for (std::size_t i = 0; i < records.size(); ++i) {
    last[records[i].file] = i;
  }
  for (std::size_t i = 0; i < records.size(); ++i) {
    const UsnRecord& record = records[i];
    if (last[record.file] != i) {
      continue;
    }
    if ((record.reason & usn_reason::file_delete) != 0) {
      remove(record.file);
    } else if (Result<void> refreshed = refresh(record.file, volume, 0); !refreshed) {
      return refreshed;
    }
  }
  return {};
}

Bytes VolumeIndex::size_on_disk() const {
  return nodes_.empty() ? Bytes{} : Bytes{nodes_[root_node_].size_on_disk};
}

std::size_t VolumeIndex::memory_bytes() const {
  return (nodes_.capacity() * sizeof(Node)) + names_.capacity() +
         (first_node_.capacity() * sizeof(std::uint32_t));
}

std::vector<FolderSize> VolumeIndex::folder_sizes(Bytes min_size_on_disk) const {
  std::vector<FolderSize> folders;
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    if (is_folder(node) && nodes_[node].size_on_disk >= min_size_on_disk.count()) {
      folders.push_back(FolderSize{.path = path_of(node),
                                   .size_on_disk = Bytes{nodes_[node].size_on_disk},
                                   .size = Bytes{nodes_[node].size}});
    }
  }
  std::ranges::sort(folders, {}, &FolderSize::path);
  return folders;
}

VolumeIndex::Children VolumeIndex::children() const {
  Children children{.offsets = std::vector<std::uint32_t>(nodes_.size() + 1, 0)};
  for (const Node& node : nodes_) {
    if ((node.flags & flag_dead) == 0 && node.parent != no_node) {
      ++children.offsets[std::size_t{node.parent} + 1];
    }
  }
  for (std::size_t i = 1; i < children.offsets.size(); ++i) {
    children.offsets[i] += children.offsets[i - 1];
  }
  children.nodes.resize(children.offsets.back());
  std::vector<std::uint32_t> next(children.offsets.begin(), children.offsets.end() - 1);
  for (std::uint32_t node = 0; node < nodes_.size(); ++node) {
    if (live(node) && nodes_[node].parent != no_node) {
      children.nodes[next[nodes_[node].parent]++] = node;
    }
  }
  return children;
}

Result<std::uint32_t> VolumeIndex::find(const fs::path& folder, const Children& children) const {
  const fs::path normal = folder.lexically_normal();
  if (!normal.has_root_directory() ||
      !equal_ignoring_ascii_case(space::utf8(normal.root_name()), space::utf8(root_.root_name()))) {
    return fail(Errc::not_found, "not on the indexed volume: " + space::utf8(folder));
  }
  std::uint32_t node = root_node_;
  for (const fs::path& part : normal.relative_path()) {
    const std::string name = space::utf8(part);
    if (name.empty()) {
      continue;
    }
    std::uint32_t match = no_node;
    for (std::uint32_t i = children.offsets[node]; i < children.offsets[std::size_t{node} + 1];
         ++i) {
      const std::uint32_t child = children.nodes[i];
      if (name_of(child) == name) {
        match = child;
        break;
      }
      if (match == no_node && equal_ignoring_ascii_case(name_of(child), name)) {
        match = child;
      }
    }
    if (match == no_node) {
      return fail(Errc::not_found, "not in the index: " + space::utf8(folder));
    }
    node = match;
  }
  if (!is_folder(node)) {
    return fail(Errc::invalid_argument, "not a folder: " + space::utf8(folder));
  }
  return node;
}

VolumeIndex::FileLists VolumeIndex::list_tree(std::uint32_t start, const Children& children,
                                              space::ScanResult& result) const {
  const auto by_name = [this](std::uint32_t left, std::uint32_t right) {
    return name_of(left) < name_of(right);
  };
  result.folders.push_back(space::Folder{.path = path_of(start)});
  std::vector<std::uint32_t> folder_nodes{start};
  FileLists files;
  for (std::size_t index = 0; index < folder_nodes.size(); ++index) {
    std::vector<std::uint32_t> subfolders;
    std::vector<std::uint32_t> own;
    const std::uint32_t node = folder_nodes[index];
    for (std::uint32_t i = children.offsets[node]; i < children.offsets[std::size_t{node} + 1];
         ++i) {
      (is_folder(children.nodes[i]) ? subfolders : own).push_back(children.nodes[i]);
    }
    std::ranges::sort(subfolders, by_name);
    std::ranges::sort(own, by_name);
    for (const std::uint32_t subfolder : subfolders) {
      fs::path path = result.folders[index].path / path_from_utf8(name_of(subfolder));
      result.folders[index].folders.push_back(result.folders.size());
      result.folders.push_back(space::Folder{.path = std::move(path), .parent = index});
      folder_nodes.push_back(subfolder);
    }
    files.push_back(std::move(own));
  }
  return files;
}

std::vector<bool> VolumeIndex::extra_links(const space::ScanResult& result,
                                           const FileLists& files) const {
  std::vector<bool> extra(nodes_.size(), false);
  std::unordered_set<std::uint32_t> seen_records;
  std::vector<std::size_t> stack{0};
  while (!stack.empty()) {
    const std::size_t index = stack.back();
    stack.pop_back();
    for (const std::uint32_t node : files[index]) {
      if (!is_link(node) && several_links(node) &&
          !seen_records.insert(nodes_[node].record).second) {
        extra[node] = true;
      }
    }
    const std::vector<std::size_t>& subfolders = result.folders[index].folders;
    stack.insert(stack.end(), subfolders.rbegin(), subfolders.rend());
  }
  return extra;
}

space::FileItem VolumeIndex::file_item(std::uint32_t node, bool extra_link) const {
  const Node& file = nodes_[node];
  const bool link = is_link(node);
  FileIdentity identity{.volume_serial = serial_};
  const std::uint64_t reference = reference_of(file.record, file.sequence);
  std::memcpy(identity.file_id.data(), &reference, sizeof(reference));
  return space::FileItem{.name = path_from_utf8(name_of(node)),
                         .link = link,
                         .placeholder = (file.flags & flag_placeholder) != 0,
                         .extra_link = extra_link,
                         .reparse_tag = link ? static_cast<std::uint32_t>(file.size) : 0,
                         .size = Bytes{link ? 0 : file.size},
                         .size_on_disk = Bytes{file.size_on_disk},
                         .created = wall_time(file.created),
                         .modified = wall_time(file.modified),
                         .identity = identity};
}

Result<space::ScanResult> VolumeIndex::to_scan_result(const fs::path& folder,
                                                      bool with_files) const {
  if (nodes_.empty()) {
    return fail(Errc::invalid_state, "the index is empty");
  }
  const Children tree = children();
  const Result<std::uint32_t> start = find(folder, tree);
  if (!start) {
    return tl::unexpected(start.error());
  }
  space::ScanResult result;
  const FileLists files = list_tree(*start, tree, result);
  const std::vector<bool> extra = extra_links(result, files);

  // Totals bottom-up, as scanner v1 adds them.
  for (std::size_t index = result.folders.size(); index-- > 0;) {
    space::Folder& current = result.folders[index];
    for (const std::uint32_t node : files[index]) {
      space::FileItem item = file_item(node, extra[node]);
      if (item.link) {
        ++result.link_count;
      } else {
        ++current.file_count;
        result.placeholder_count += item.placeholder ? 1 : 0;
        if (!item.extra_link) {
          current.own_size_on_disk += item.size_on_disk;
          current.size += item.size;
        }
      }
      if (with_files) {
        current.files.push_back(std::move(item));
      }
    }
    current.size_on_disk += current.own_size_on_disk;
    if (current.parent != space::no_parent) {
      space::Folder& parent = result.folders[current.parent];
      parent.size_on_disk += current.size_on_disk;
      parent.size += current.size;
      parent.file_count += current.file_count;
      parent.folder_count += current.folder_count + 1;
    }
  }
  return result;
}

}  // namespace myclean::index
