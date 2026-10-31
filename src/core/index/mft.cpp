#include "core/index/mft.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/index/bytes.hpp"
#include "core/model/result.hpp"

namespace myclean::index {

namespace {

// The update sequence protects the last two bytes of every 512-byte stride of a record.
constexpr std::size_t stride = 512;
constexpr std::size_t header_size = 0x30;
constexpr std::uint32_t end_marker = 0xFFFFFFFFU;

constexpr std::uint16_t flag_in_use = 0x0001;
constexpr std::uint16_t flag_directory = 0x0002;

constexpr std::uint32_t type_standard_information = 0x10;
constexpr std::uint32_t type_attribute_list = 0x20;
constexpr std::uint32_t type_file_name = 0x30;
constexpr std::uint32_t type_data = 0x80;
constexpr std::uint32_t type_reparse_point = 0xC0;

constexpr std::uint16_t attribute_compressed = 0x0001;
constexpr std::uint16_t attribute_sparse = 0x8000;

constexpr std::uint8_t namespace_dos = 2;

constexpr std::size_t resident_header = 0x18;
constexpr std::size_t non_resident_header = 0x40;
// A non-resident header this long carries the total allocation (compressed or sparse streams).
constexpr std::size_t compressed_header = 0x48;

Error damaged(std::string_view what) {
  return Error{.code = Errc::invalid_data, .message = "damaged FILE record: " + std::string(what)};
}

// Restores the original last two bytes of every stride, or accepts them when they already are.
Result<void> apply_fixups(std::span<std::byte> bytes) {
  const auto offset = load<std::uint16_t>(bytes, 4);
  const auto count = load<std::uint16_t>(bytes, 6);
  if (count < 2 || !fits(bytes, offset, std::size_t{count} * 2) ||
      (std::size_t{count} - 1) * stride > bytes.size()) {
    return tl::unexpected(damaged("update sequence out of range"));
  }
  const auto sequence = load<std::uint16_t>(bytes, offset);
  for (std::size_t sector = 1; sector < count; ++sector) {
    const std::size_t end = (sector * stride) - 2;
    const auto original = load<std::uint16_t>(bytes, offset + (sector * 2));
    const auto current = load<std::uint16_t>(bytes, end);
    if (current == sequence) {
      store(bytes, end, original);
    } else if (current != original) {
      return tl::unexpected(damaged("torn write (update sequence mismatch)"));
    }
  }
  return {};
}

void read_standard_information(std::span<const std::byte> value, RecordSegment& out) {
  if (value.size() < 0x24) {
    return;
  }
  out.has_standard_information = true;
  out.created = load<std::uint64_t>(value, 0x00);
  out.modified = load<std::uint64_t>(value, 0x08);
  out.attributes = load<std::uint32_t>(value, 0x20);
}

Result<void> read_file_name(std::span<const std::byte> value, RecordSegment& out) {
  constexpr std::size_t name_offset = 0x42;
  if (value.size() < name_offset) {
    return tl::unexpected(damaged("short $FILE_NAME"));
  }
  const auto length = static_cast<std::size_t>(load<std::uint8_t>(value, 0x40));
  const auto name_space = load<std::uint8_t>(value, 0x41);
  if (!fits(value, name_offset, length * 2)) {
    return tl::unexpected(damaged("$FILE_NAME name out of range"));
  }
  // The field holds the reparse tag only for a reparse point (otherwise the size of the EAs).
  if (out.reparse_tag == 0 && (out.attributes & file_attribute::reparse_point) != 0) {
    out.reparse_tag = load<std::uint32_t>(value, 0x3C);
  }
  if (name_space != namespace_dos) {
    out.names.push_back(FileName{.parent = load<std::uint64_t>(value, 0x00),
                                 .name = utf8_of_utf16(value.subspan(name_offset, length * 2))});
  }
  return {};
}

// One attribute header, already bounds-checked against the record.
struct Attribute {
  std::uint32_t type = 0;
  bool resident = true;
  bool named = false;
  std::uint16_t flags = 0;
  std::span<const std::byte> header;  // the whole attribute
  std::span<const std::byte> value;   // resident value
};

Result<Attribute> attribute_at(std::span<const std::byte> attribute) {
  Attribute result{.type = load<std::uint32_t>(attribute, 0),
                   .resident = load<std::uint8_t>(attribute, 8) == 0,
                   .named = load<std::uint8_t>(attribute, 9) != 0,
                   .flags = load<std::uint16_t>(attribute, 0x0C),
                   .header = attribute};
  if (result.resident) {
    if (attribute.size() < resident_header) {
      return tl::unexpected(damaged("short resident attribute"));
    }
    const auto length = load<std::uint32_t>(attribute, 0x10);
    const auto offset = load<std::uint16_t>(attribute, 0x14);
    if (!fits(attribute, offset, length)) {
      return tl::unexpected(damaged("resident value out of range"));
    }
    result.value = attribute.subspan(offset, length);
  } else if (attribute.size() < non_resident_header) {
    return tl::unexpected(damaged("short non-resident attribute"));
  }
  return result;
}

Result<std::vector<Extent>> runs_of(const Attribute& attribute) {
  const auto offset = load<std::uint16_t>(attribute.header, 0x20);
  if (offset > attribute.header.size()) {
    return tl::unexpected(damaged("mapping pairs out of range"));
  }
  return decode_runs(attribute.header.subspan(offset), load<std::uint64_t>(attribute.header, 0x10));
}

Result<void> read_data(const Attribute& attribute, RecordSegment& out, bool with_extents) {
  if (attribute.resident) {
    if (!attribute.named) {
      out.size = attribute.value.size();
    }
    return {};
  }
  const std::span<const std::byte> header = attribute.header;
  if (load<std::uint64_t>(header, 0x10) == 0) {
    const bool counts_total = (attribute.flags & (attribute_compressed | attribute_sparse)) != 0 &&
                              load<std::uint16_t>(header, 0x20) >= compressed_header &&
                              header.size() >= compressed_header;
    out.size_on_disk += load<std::uint64_t>(header, counts_total ? 0x40 : 0x28);
    if (!attribute.named) {
      out.size = load<std::uint64_t>(header, 0x30);
    }
  }
  if (with_extents && !attribute.named) {
    Result<std::vector<Extent>> runs = runs_of(attribute);
    if (!runs) {
      return tl::unexpected(runs.error());
    }
    out.data_extents.insert(out.data_extents.end(), runs->begin(), runs->end());
  }
  return {};
}

Result<void> read_attribute_list(const Attribute& attribute, RecordSegment& out) {
  out.has_attribute_list = true;
  if (attribute.resident) {
    out.attribute_list.assign(attribute.value.begin(), attribute.value.end());
    return {};
  }
  Result<std::vector<Extent>> runs = runs_of(attribute);
  if (!runs) {
    return tl::unexpected(runs.error());
  }
  out.attribute_list_extents = std::move(*runs);
  out.attribute_list_length = load<std::uint64_t>(attribute.header, 0x30);
  return {};
}

Result<void> read_attribute(const Attribute& attribute, RecordSegment& out, bool with_extents) {
  switch (attribute.type) {
    case type_standard_information:
      if (attribute.resident) {
        read_standard_information(attribute.value, out);
      }
      return {};
    case type_attribute_list:
      return read_attribute_list(attribute, out);
    case type_file_name:
      return attribute.resident ? read_file_name(attribute.value, out) : Result<void>{};
    case type_data:
      return read_data(attribute, out, with_extents);
    case type_reparse_point:
      if (attribute.resident && attribute.value.size() >= 4) {
        out.reparse_tag = load<std::uint32_t>(attribute.value, 0);
      }
      return {};
    default:
      return {};
  }
}

void clear(RecordSegment& out) {
  out.record = 0;
  out.sequence = 0;
  out.base = 0;
  out.in_use = false;
  out.directory = false;
  out.has_standard_information = false;
  out.attributes = 0;
  out.created = 0;
  out.modified = 0;
  out.reparse_tag = 0;
  out.names.clear();
  out.size.reset();
  out.size_on_disk = 0;
  out.data_extents.clear();
  out.has_attribute_list = false;
  out.attribute_list.clear();
  out.attribute_list_extents.clear();
  out.attribute_list_length = 0;
}

}  // namespace

Result<void> parse_record(std::span<std::byte> bytes, RecordSegment& out, bool with_extents) {
  clear(out);
  if (bytes.size() < header_size) {
    return tl::unexpected(damaged("shorter than its header"));
  }
  constexpr std::array<std::byte, 4> signature{std::byte{'F'}, std::byte{'I'}, std::byte{'L'},
                                               std::byte{'E'}};
  if (!std::ranges::equal(bytes.first(4), signature)) {
    if (std::ranges::all_of(bytes.first(4), [](std::byte b) { return b == std::byte{0}; })) {
      return {};  // never used
    }
    return tl::unexpected(damaged("bad signature"));
  }
  if (Result<void> fixed = apply_fixups(bytes); !fixed) {
    return fixed;
  }
  const std::span<const std::byte> record = bytes;
  const auto flags = load<std::uint16_t>(record, 0x16);
  out.sequence = load<std::uint16_t>(record, 0x10);
  out.base = load<std::uint64_t>(record, 0x20);
  out.record = load<std::uint32_t>(record, 0x2C);
  out.in_use = (flags & flag_in_use) != 0;
  out.directory = (flags & flag_directory) != 0;
  if (!out.in_use) {
    return {};
  }

  const std::size_t used = std::min<std::size_t>(load<std::uint32_t>(record, 0x18), record.size());
  std::size_t offset = load<std::uint16_t>(record, 0x14);
  while (fits(record.first(used), offset, 4)) {
    const auto type = load<std::uint32_t>(record, offset);
    if (type == end_marker) {
      return {};
    }
    if (!fits(record.first(used), offset, 8)) {
      break;
    }
    const auto length = load<std::uint32_t>(record, offset + 4);
    if (length < 0x10 || !fits(record.first(used), offset, length)) {
      return tl::unexpected(damaged("attribute length out of range"));
    }
    const Result<Attribute> attribute = attribute_at(record.subspan(offset, length));
    if (!attribute) {
      return tl::unexpected(attribute.error());
    }
    if (Result<void> read = read_attribute(*attribute, out, with_extents); !read) {
      return read;
    }
    offset += length;
  }
  return tl::unexpected(damaged("no end marker"));
}

Result<std::vector<Extent>> decode_runs(std::span<const std::byte> pairs, std::uint64_t first_vcn) {
  std::vector<Extent> extents;
  std::uint64_t vcn = first_vcn;
  std::int64_t lcn = 0;
  std::size_t offset = 0;
  while (offset < pairs.size()) {
    const auto header = static_cast<unsigned>(pairs[offset]);
    if (header == 0) {
      return extents;
    }
    const unsigned length_bytes = header & 0x0FU;
    const unsigned offset_bytes = header >> 4U;
    if (length_bytes == 0 || length_bytes > 8 || offset_bytes > 8 ||
        !fits(pairs, offset + 1, std::size_t{length_bytes} + offset_bytes)) {
      return fail(Errc::invalid_data, "damaged mapping pairs");
    }
    std::uint64_t clusters = 0;
    for (unsigned i = 0; i < length_bytes; ++i) {
      clusters |= static_cast<std::uint64_t>(pairs[offset + 1 + i]) << (8U * i);
    }
    Extent extent{.vcn = vcn, .clusters = clusters};
    if (offset_bytes != 0) {
      std::uint64_t delta = 0;
      for (unsigned i = 0; i < offset_bytes; ++i) {
        delta |= static_cast<std::uint64_t>(pairs[offset + 1 + length_bytes + i]) << (8U * i);
      }
      const unsigned unused_bits = 64U - (8U * offset_bytes);
      // Sign-extend the little-endian delta from its top byte.
      const auto signed_delta =
          unused_bits == 0 ? static_cast<std::int64_t>(delta)
                           : static_cast<std::int64_t>(delta << unused_bits) >> unused_bits;
      lcn += signed_delta;
      if (lcn < 0) {
        return fail(Errc::invalid_data, "mapping pairs point before the volume start");
      }
      extent.lcn = static_cast<std::uint64_t>(lcn);
    }
    if (clusters == 0) {
      return fail(Errc::invalid_data, "empty run in mapping pairs");
    }
    extents.push_back(extent);
    vcn += clusters;
    offset += 1 + length_bytes + offset_bytes;
  }
  return extents;
}

Result<std::vector<std::uint64_t>> extension_records(std::span<const std::byte> attribute_list,
                                                     std::uint64_t base_record) {
  constexpr std::size_t entry_header = 0x1A;
  std::vector<std::uint64_t> records;
  std::size_t offset = 0;
  while (fits(attribute_list, offset, entry_header)) {
    const auto length = load<std::uint16_t>(attribute_list, offset + 4);
    if (length < entry_header || !fits(attribute_list, offset, length)) {
      return fail(Errc::invalid_data, "damaged attribute list entry");
    }
    const std::uint64_t record = record_of(load<std::uint64_t>(attribute_list, offset + 0x10));
    if (record != base_record && std::ranges::find(records, record) == records.end()) {
      records.push_back(record);
    }
    offset += length;
  }
  return records;
}

std::string utf8_of_utf16(std::span<const std::byte> bytes) {
  constexpr char32_t replacement = 0xFFFD;
  std::string text;
  text.reserve(bytes.size() / 2);
  const std::size_t units = bytes.size() / 2;
  for (std::size_t i = 0; i < units; ++i) {
    char32_t code = load<std::uint16_t>(bytes, i * 2);
    if (code >= 0xD800 && code <= 0xDBFF && i + 1 < units) {
      const char32_t low = load<std::uint16_t>(bytes, (i + 1) * 2);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        code = 0x10000 + ((code - 0xD800) << 10U) + (low - 0xDC00);
        ++i;
      } else {
        code = replacement;
      }
    } else if (code >= 0xD800 && code <= 0xDFFF) {
      code = replacement;
    }
    if (code < 0x80) {
      text.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      text.push_back(static_cast<char>(0xC0U | (code >> 6U)));
      text.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code < 0x10000) {
      text.push_back(static_cast<char>(0xE0U | (code >> 12U)));
      text.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      text.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
      text.push_back(static_cast<char>(0xF0U | (code >> 18U)));
      text.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
      text.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      text.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
  }
  return text;
}

}  // namespace myclean::index
