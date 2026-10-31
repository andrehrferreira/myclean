#include "core/index/usn.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/index/bytes.hpp"
#include "core/index/mft.hpp"
#include "core/model/result.hpp"

namespace myclean::index {

namespace {

// Field offsets that differ between USN_RECORD_V2 (64-bit ids) and USN_RECORD_V3 (128-bit ids).
struct Layout {
  std::size_t id_size;
  std::size_t usn;  // the parent id is at 8 + id_size, the fields below follow USN
  std::size_t reason;
  std::size_t attributes;
  std::size_t name_length;
  std::size_t name_offset;
};

constexpr Layout v2{.id_size = 8,
                    .usn = 0x18,
                    .reason = 0x28,
                    .attributes = 0x34,
                    .name_length = 0x38,
                    .name_offset = 0x3A};
constexpr Layout v3{.id_size = 16,
                    .usn = 0x28,
                    .reason = 0x38,
                    .attributes = 0x44,
                    .name_length = 0x48,
                    .name_offset = 0x4A};

Result<std::uint64_t> id_at(std::span<const std::byte> record, std::size_t offset,
                            std::size_t size) {
  if (size == 16 && load<std::uint64_t>(record, offset + 8) != 0) {
    return fail(Errc::invalid_data, "USN record file id does not fit in 64 bits");
  }
  return load<std::uint64_t>(record, offset);
}

Result<UsnRecord> parse_one(std::span<const std::byte> record, const Layout& layout) {
  if (record.size() < layout.name_offset + 2) {
    return fail(Errc::invalid_data, "short USN record");
  }
  const auto name_length = load<std::uint16_t>(record, layout.name_length);
  const auto name_offset = load<std::uint16_t>(record, layout.name_offset);
  if (!fits(record, name_offset, name_length)) {
    return fail(Errc::invalid_data, "USN record name out of range");
  }
  const Result<std::uint64_t> file = id_at(record, 8, layout.id_size);
  const Result<std::uint64_t> parent = id_at(record, 8 + layout.id_size, layout.id_size);
  if (!file || !parent) {
    return tl::unexpected(file ? parent.error() : file.error());
  }
  return UsnRecord{.file = *file,
                   .parent = *parent,
                   .usn = load<std::int64_t>(record, layout.usn),
                   .reason = load<std::uint32_t>(record, layout.reason),
                   .attributes = load<std::uint32_t>(record, layout.attributes),
                   .name = utf8_of_utf16(record.subspan(name_offset, name_length))};
}

}  // namespace

Result<std::vector<UsnRecord>> parse_usn_records(std::span<const std::byte> bytes) {
  std::vector<UsnRecord> records;
  std::size_t offset = 0;
  while (fits(bytes, offset, 8)) {
    const auto length = load<std::uint32_t>(bytes, offset);
    const auto major = load<std::uint16_t>(bytes, offset + 4);
    if (length < 8 || !fits(bytes, offset, length)) {
      return fail(Errc::invalid_data, "USN record length out of range");
    }
    const std::span<const std::byte> record = bytes.subspan(offset, length);
    if (major == 2 || major == 3) {
      Result<UsnRecord> parsed = parse_one(record, major == 2 ? v2 : v3);
      if (!parsed) {
        return tl::unexpected(parsed.error());
      }
      records.push_back(std::move(*parsed));
    } else if (major != 4) {
      return fail(Errc::invalid_data, "unknown USN record version " + std::to_string(major));
    }
    offset += (std::size_t{length} + 7) & ~std::size_t{7};
  }
  return records;
}

Result<UsnBuffer> parse_usn_buffer(std::span<const std::byte> bytes) {
  if (bytes.size() < 8) {
    return fail(Errc::invalid_data, "USN buffer shorter than its header");
  }
  Result<std::vector<UsnRecord>> records = parse_usn_records(bytes.subspan(8));
  if (!records) {
    return tl::unexpected(records.error());
  }
  return UsnBuffer{.next = load<std::uint64_t>(bytes, 0), .records = std::move(*records)};
}

}  // namespace myclean::index
