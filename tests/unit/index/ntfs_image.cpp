#include "ntfs_image.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "core/index/bytes.hpp"
#include "core/index/mft.hpp"
#include "core/index/volume.hpp"
#include "core/model/result.hpp"

namespace myclean::index::testing {

namespace {

constexpr std::size_t stride = 512;

constexpr std::size_t align8(std::size_t value) {
  return (value + 7) & ~std::size_t{7};
}

template <typename T>
void put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
  store(std::span{bytes}, offset, value);
}

void put_name(std::vector<std::byte>& bytes, std::size_t offset, std::u16string_view name) {
  for (std::size_t i = 0; i < name.size(); ++i) {
    put(bytes, offset + (i * 2), static_cast<std::uint16_t>(name[i]));
  }
}

std::vector<std::byte> resident(std::uint32_t type, std::u16string_view name,
                                std::span<const std::byte> value) {
  constexpr std::size_t name_offset = 0x18;
  const std::size_t value_offset = align8(name_offset + (name.size() * 2));
  const std::size_t length = align8(value_offset + value.size());
  std::vector<std::byte> bytes(length);
  put(bytes, 0x00, type);
  put(bytes, 0x04, static_cast<std::uint32_t>(length));
  put(bytes, 0x09, static_cast<std::uint8_t>(name.size()));
  put(bytes, 0x0A, static_cast<std::uint16_t>(name_offset));
  put(bytes, 0x10, static_cast<std::uint32_t>(value.size()));
  put(bytes, 0x14, static_cast<std::uint16_t>(value_offset));
  put_name(bytes, name_offset, name);
  std::ranges::copy(value, std::span{bytes}.subspan(value_offset).begin());
  return bytes;
}

struct NonResident {
  std::uint32_t type = 0;
  std::u16string_view name;
  std::uint16_t flags = 0;
  std::uint64_t lowest_vcn = 0;
  std::vector<Extent> runs;
  std::uint64_t allocated = 0;
  std::uint64_t size = 0;
  std::optional<std::uint64_t> total;
};

std::vector<std::byte> non_resident(const NonResident& spec) {
  const std::size_t header = spec.total ? 0x48 : 0x40;
  const std::vector<std::byte> pairs = encode_runs(spec.runs);
  const std::size_t pairs_offset = align8(header + (spec.name.size() * 2));
  const std::size_t length = align8(pairs_offset + pairs.size());
  std::uint64_t clusters = 0;
  for (const Extent& extent : spec.runs) {
    clusters += extent.clusters;
  }
  std::vector<std::byte> bytes(length);
  put(bytes, 0x00, spec.type);
  put(bytes, 0x04, static_cast<std::uint32_t>(length));
  put(bytes, 0x08, std::uint8_t{1});
  put(bytes, 0x09, static_cast<std::uint8_t>(spec.name.size()));
  put(bytes, 0x0A, static_cast<std::uint16_t>(header));
  put(bytes, 0x0C, spec.flags);
  put(bytes, 0x10, spec.lowest_vcn);
  put(bytes, 0x18, clusters == 0 ? spec.lowest_vcn : spec.lowest_vcn + clusters - 1);
  put(bytes, 0x20, static_cast<std::uint16_t>(pairs_offset));
  put(bytes, 0x22, static_cast<std::uint8_t>((spec.flags & compressed) != 0 ? 4 : 0));
  put(bytes, 0x28, spec.allocated);
  put(bytes, 0x30, spec.size);
  put(bytes, 0x38, spec.size);
  if (spec.total) {
    put(bytes, 0x40, *spec.total);
  }
  put_name(bytes, header, spec.name);
  std::ranges::copy(pairs, std::span{bytes}.subspan(pairs_offset).begin());
  return bytes;
}

unsigned unsigned_width(std::uint64_t value) {
  unsigned width = 1;
  while (width < 8 && (value >> (8U * width)) != 0) {
    ++width;
  }
  return width;
}

unsigned signed_width(std::int64_t value) {
  for (unsigned width = 1; width < 8; ++width) {
    const std::int64_t limit = std::int64_t{1} << ((8U * width) - 1);
    if (value >= -limit && value < limit) {
      return width;
    }
  }
  return 8;
}

std::vector<std::byte> usn_record(unsigned version, std::uint64_t file, std::uint64_t parent,
                                  std::int64_t usn, std::uint32_t reason, std::uint32_t attributes,
                                  std::u16string_view name) {
  const std::size_t id = version == 2 ? 8 : 16;
  const std::size_t after_ids = 8 + (2 * id);  // USN follows the two ids
  const std::size_t name_offset = after_ids + 0x24;
  const std::size_t length = align8(name_offset + (name.size() * 2));
  std::vector<std::byte> bytes(length);
  put(bytes, 0x00, static_cast<std::uint32_t>(length));
  put(bytes, 0x04, static_cast<std::uint16_t>(version));
  put(bytes, 0x08, file);
  put(bytes, 0x08 + id, parent);
  put(bytes, after_ids, usn);
  put(bytes, after_ids + 0x08, file_time);
  put(bytes, after_ids + 0x10, reason);
  put(bytes, after_ids + 0x1C, attributes);
  put(bytes, after_ids + 0x20, static_cast<std::uint16_t>(name.size() * 2));
  put(bytes, after_ids + 0x22, static_cast<std::uint16_t>(name_offset));
  put_name(bytes, name_offset, name);
  return bytes;
}

}  // namespace

std::vector<std::byte> encode_runs(std::span<const Extent> extents) {
  std::vector<std::byte> pairs;
  std::int64_t previous = 0;
  for (const Extent& extent : extents) {
    const unsigned length_width = unsigned_width(extent.clusters);
    std::int64_t delta = 0;
    unsigned offset_width = 0;
    if (extent.lcn) {
      delta = static_cast<std::int64_t>(*extent.lcn) - previous;
      previous = static_cast<std::int64_t>(*extent.lcn);
      offset_width = signed_width(delta);
    }
    pairs.push_back(static_cast<std::byte>((offset_width << 4U) | length_width));
    for (unsigned i = 0; i < length_width; ++i) {
      pairs.push_back(static_cast<std::byte>(extent.clusters >> (8U * i)));
    }
    for (unsigned i = 0; i < offset_width; ++i) {
      pairs.push_back(static_cast<std::byte>(static_cast<std::uint64_t>(delta) >> (8U * i)));
    }
  }
  pairs.push_back(std::byte{0});
  return pairs;
}

RecordBuilder& RecordBuilder::directory() {
  directory_ = true;
  return *this;
}

RecordBuilder& RecordBuilder::not_in_use() {
  in_use_ = false;
  return *this;
}

RecordBuilder& RecordBuilder::base(std::uint64_t reference) {
  base_ = reference;
  return *this;
}

RecordBuilder& RecordBuilder::standard_information(std::uint32_t attributes,
                                                   std::uint64_t modified) {
  std::vector<std::byte> value(0x48);
  put(value, 0x00, file_time);
  put(value, 0x08, modified);
  put(value, 0x10, modified);
  put(value, 0x18, modified);
  put(value, 0x20, attributes);
  attributes_.push_back(resident(0x10, u"", value));
  return *this;
}

RecordBuilder& RecordBuilder::file_name(std::uint64_t parent, std::u16string_view name,
                                        std::uint8_t name_space, std::uint32_t reparse_tag) {
  std::vector<std::byte> value(0x42 + (name.size() * 2));
  put(value, 0x00, parent);
  put(value, 0x08, file_time);
  put(value, 0x3C, reparse_tag);
  put(value, 0x40, static_cast<std::uint8_t>(name.size()));
  put(value, 0x41, name_space);
  put_name(value, 0x42, name);
  attributes_.push_back(resident(0x30, u"", value));
  return *this;
}

RecordBuilder& RecordBuilder::resident_data(std::u16string_view stream, std::uint64_t size) {
  const std::vector<std::byte> value(size, std::byte{0x5A});
  attributes_.push_back(resident(0x80, stream, value));
  return *this;
}

RecordBuilder& RecordBuilder::data(std::u16string_view stream, std::uint64_t size,
                                   std::uint64_t allocated, std::vector<Extent> runs,
                                   std::uint16_t flags,
                                   std::optional<std::uint64_t> total_allocated) {
  if (runs.empty() && allocated != 0) {
    runs.push_back(Extent{.lcn = 4000, .clusters = (allocated + cluster_size - 1) / cluster_size});
  }
  attributes_.push_back(non_resident(NonResident{.type = 0x80,
                                                 .name = stream,
                                                 .flags = flags,
                                                 .runs = std::move(runs),
                                                 .allocated = allocated,
                                                 .size = size,
                                                 .total = total_allocated}));
  return *this;
}

RecordBuilder& RecordBuilder::data_part(std::uint64_t lowest_vcn, std::vector<Extent> runs) {
  attributes_.push_back(
      non_resident(NonResident{.type = 0x80, .lowest_vcn = lowest_vcn, .runs = std::move(runs)}));
  return *this;
}

RecordBuilder& RecordBuilder::reparse_point(std::uint32_t tag) {
  std::vector<std::byte> value(8);
  put(value, 0, tag);
  attributes_.push_back(resident(0xC0, u"", value));
  return *this;
}

RecordBuilder& RecordBuilder::attribute_list(std::span<const std::byte> value) {
  attributes_.push_back(resident(0x20, u"", value));
  return *this;
}

RecordBuilder& RecordBuilder::attribute_list(std::vector<Extent> runs, std::uint64_t length) {
  attributes_.push_back(non_resident(
      NonResident{.type = 0x20, .runs = std::move(runs), .allocated = length, .size = length}));
  return *this;
}

std::vector<std::byte> RecordBuilder::build(std::optional<std::uint16_t> usn) const {
  std::vector<std::byte> record(record_size);
  const std::uint16_t count = (record_size / stride) + 1;
  const std::size_t first = align8(0x30 + (std::size_t{count} * 2));
  constexpr std::string_view signature = "FILE";
  for (std::size_t i = 0; i < signature.size(); ++i) {
    record[i] = static_cast<std::byte>(signature[i]);
  }
  put(record, 0x04, std::uint16_t{0x30});
  put(record, 0x06, count);
  put(record, 0x10, sequence_);
  put(record, 0x12, std::uint16_t{1});
  put(record, 0x14, static_cast<std::uint16_t>(first));
  put(record, 0x16,
      static_cast<std::uint16_t>((in_use_ ? 0x0001U : 0U) | (directory_ ? 0x0002U : 0U)));
  std::size_t offset = first;
  for (const std::vector<std::byte>& attribute : attributes_) {
    if (offset + attribute.size() + 8 > record_size) {
      throw std::logic_error("attributes do not fit in a FILE record");
    }
    std::ranges::copy(attribute, std::span{record}.subspan(offset).begin());
    offset += attribute.size();
  }
  put(record, offset, std::uint32_t{0xFFFFFFFFU});
  offset += 8;
  put(record, 0x18, static_cast<std::uint32_t>(offset));
  put(record, 0x1C, record_size);
  put(record, 0x20, base_);
  put(record, 0x28, static_cast<std::uint16_t>(attributes_.size()));
  put(record, 0x2C, static_cast<std::uint32_t>(record_));

  const std::uint16_t number = usn.value_or(0x0042);
  put(record, 0x30, number);
  for (std::size_t sector = 1; sector < count; ++sector) {
    const std::size_t end = (sector * stride) - 2;
    put(record, 0x30 + (sector * 2), load<std::uint16_t>(record, end));
    if (usn) {
      put(record, end, *usn);
    }
  }
  return record;
}

std::vector<std::byte> RecordBuilder::on_disk(std::uint16_t usn) const {
  return build(usn);
}

std::vector<std::byte> RecordBuilder::restored() const {
  return build(std::nullopt);
}

std::vector<std::byte> list_entry(std::uint32_t type, std::uint64_t segment,
                                  std::uint64_t lowest_vcn) {
  std::vector<std::byte> entry(0x20);
  put(entry, 0x00, type);
  put(entry, 0x04, std::uint16_t{0x20});
  put(entry, 0x07, std::uint8_t{0x1A});
  put(entry, 0x08, lowest_vcn);
  put(entry, 0x10, segment);
  return entry;
}

std::vector<std::byte> usn_v2(std::uint64_t file, std::uint64_t parent, std::int64_t usn,
                              std::uint32_t reason, std::uint32_t attributes,
                              std::u16string_view name) {
  return usn_record(2, file, parent, usn, reason, attributes, name);
}

std::vector<std::byte> usn_v3(std::uint64_t file, std::uint64_t parent, std::int64_t usn,
                              std::uint32_t reason, std::uint32_t attributes,
                              std::u16string_view name) {
  return usn_record(3, file, parent, usn, reason, attributes, name);
}

FakeVolume::FakeVolume(std::uint64_t capacity) : capacity_(capacity) {
  const std::uint64_t clusters = (capacity * record_size) / cluster_size;
  const std::uint64_t half = clusters / 2;
  runs_ = {Extent{.vcn = 0, .lcn = 100, .clusters = half},
           Extent{.vcn = half, .lcn = 100 + half + 50, .clusters = clusters - half}};
  disk_.resize((100 + clusters + 60) * cluster_size);
  const std::uint64_t mft_bytes = capacity * record_size;
  put(0, RecordBuilder{0}
             .standard_information(0x06)
             .file_name(root_ref, u"$MFT")
             .data(u"", mft_bytes, mft_bytes, runs_));
}

std::uint64_t FakeVolume::disk_offset(std::uint64_t record) const {
  const std::uint64_t byte = record * record_size;
  const std::uint64_t vcn = byte / cluster_size;
  for (const Extent& run : runs_) {
    if (vcn >= run.vcn && vcn < run.vcn + run.clusters) {
      return ((*run.lcn + (vcn - run.vcn)) * cluster_size) + (byte % cluster_size);
    }
  }
  throw std::logic_error("record beyond the $MFT capacity");
}

void FakeVolume::put(std::uint64_t record, const RecordBuilder& builder) {
  const std::vector<std::byte> bytes = builder.on_disk();
  std::ranges::copy(bytes, std::span{disk_}.subspan(disk_offset(record)).begin());
  std::vector<std::byte> restored = builder.restored();
  RecordSegment segment;
  std::vector<std::byte> copy = restored;
  if (parse_record(copy, segment) && segment.in_use && segment.base == 0 &&
      !segment.names.empty()) {
    const std::string& name = segment.names.front().name;
    listed_[record] = Listed{.reference = ref(record, segment.sequence),
                             .parent = segment.names.front().parent,
                             .name = std::u16string(name.begin(), name.end())};
  }
  if (segment.in_use) {
    restored_[record] = std::move(restored);
  } else {
    restored_.erase(record);
  }
}

void FakeVolume::free(std::uint64_t record) {
  put(record, RecordBuilder{record}.not_in_use());
  listed_.erase(record);
}

void FakeVolume::journal_add(std::uint64_t file, std::uint64_t parent, std::uint32_t reason,
                             std::u16string_view name) {
  if (!journal_state_.has_value()) {
    throw std::logic_error("the volume has no journal");
  }
  JournalState& state = *journal_state_;
  std::vector<std::byte> bytes = usn_v2(file, parent, state.next_usn, reason, archive, name);
  const auto length = static_cast<std::int64_t>(bytes.size());
  journal_[state.next_usn] = std::move(bytes);
  state.next_usn += length;
}

Result<VolumeGeometry> FakeVolume::geometry() const {
  return VolumeGeometry{.serial = serial,
                        .bytes_per_sector = 512,
                        .bytes_per_cluster = cluster_size,
                        .bytes_per_record = record_size,
                        .mft_start_lcn = *runs_.front().lcn,
                        .mft_valid_length = capacity_ * record_size};
}

Result<void> FakeVolume::read(std::uint64_t offset, std::span<std::byte> into) const {
  if (offset % 512 != 0 || into.size() % 512 != 0) {
    return fail(Errc::invalid_argument, "unaligned volume read");
  }
  if (offset > disk_.size() || into.size() > disk_.size() - offset) {
    return fail(Errc::io_error, "read past the end of the volume");
  }
  std::ranges::copy(std::span{disk_}.subspan(offset, into.size()), into.begin());
  return {};
}

Result<std::optional<std::vector<std::byte>>> FakeVolume::file_record(std::uint64_t record) const {
  const auto found = restored_.find(record);
  if (found == restored_.end()) {
    return std::optional<std::vector<std::byte>>{};
  }
  return std::optional<std::vector<std::byte>>{found->second};
}

Result<std::vector<std::byte>> FakeVolume::read_journal(std::uint64_t id, std::int64_t from) const {
  if (!journal_state_.has_value() || id != journal_state_->id || overwrite_while_reading_ ||
      from < journal_state_->first_usn) {
    return fail(Errc::not_found, "journal records deleted");
  }
  std::vector<std::byte> output(8);
  std::size_t taken = 0;
  auto it = journal_.lower_bound(from);
  for (; it != journal_.end() && taken < journal_records_per_read; ++it, ++taken) {
    output.insert(output.end(), it->second.begin(), it->second.end());
  }
  const std::int64_t next = it == journal_.end() ? journal_state_->next_usn : it->first;
  store(std::span{output}, 0, next);
  return output;
}

Result<std::vector<std::byte>> FakeVolume::enumerate(std::uint64_t from) const {
  auto it = listed_.lower_bound(record_of(from));
  if (it == listed_.end()) {
    return std::vector<std::byte>{};
  }
  std::vector<std::byte> output(8);
  for (; it != listed_.end(); ++it) {
    const std::vector<std::byte> record =
        usn_v2(it->second.reference, it->second.parent, 0, 0, archive, it->second.name);
    output.insert(output.end(), record.begin(), record.end());
  }
  store(std::span{output}, 0, std::uint64_t{0xFFFFFFFFFFFFULL});
  return output;
}

}  // namespace myclean::index::testing
