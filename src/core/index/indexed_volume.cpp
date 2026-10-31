#include "core/index/indexed_volume.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/index/mft.hpp"
#include "core/index/usn.hpp"
#include "core/index/volume.hpp"
#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::index {

namespace {

// Large sequential reads; a multiple of every NTFS cluster size.
constexpr std::size_t chunk_bytes = std::size_t{4} << 20U;

Result<void> check(const VolumeGeometry& geometry) {
  constexpr std::uint32_t stride = 512;
  const bool valid = geometry.bytes_per_sector != 0 && geometry.bytes_per_cluster != 0 &&
                     geometry.bytes_per_record != 0 && geometry.bytes_per_record % stride == 0 &&
                     geometry.bytes_per_cluster % geometry.bytes_per_sector == 0 &&
                     chunk_bytes % geometry.bytes_per_cluster == 0;
  if (!valid) {
    return fail(Errc::invalid_data, "unsupported NTFS geometry");
  }
  return {};
}

// The runs of the $MFT itself, from record 0 and, for a very fragmented $MFT, the extension
// segments its attribute list names.
Result<std::vector<Extent>> mft_extents(const INtfsVolume& volume, const VolumeGeometry& geometry) {
  const std::size_t sector = geometry.bytes_per_sector;
  std::vector<std::byte> first(((geometry.bytes_per_record + sector - 1) / sector) * sector);
  if (Result<void> read = volume.read(geometry.mft_start_lcn * geometry.bytes_per_cluster, first);
      !read) {
    return tl::unexpected(read.error());
  }
  RecordSegment record;
  if (Result<void> parsed =
          parse_record(std::span{first}.first(geometry.bytes_per_record), record, true);
      !parsed) {
    return tl::unexpected(parsed.error());
  }
  std::vector<Extent> extents = record.data_extents;
  if (record.has_attribute_list) {
    Result<std::vector<std::byte>> list =
        record.attribute_list_extents.empty()
            ? Result<std::vector<std::byte>>{record.attribute_list}
            : read_extents(volume, record.attribute_list_extents, record.attribute_list_length);
    if (!list) {
      return tl::unexpected(list.error());
    }
    const Result<std::vector<std::uint64_t>> extensions = extension_records(*list, 0);
    if (!extensions) {
      return tl::unexpected(extensions.error());
    }
    for (const std::uint64_t extension : *extensions) {
      Result<std::optional<std::vector<std::byte>>> bytes = volume.file_record(extension);
      if (!bytes) {
        return tl::unexpected(bytes.error());
      }
      RecordSegment part;
      if (!*bytes || !parse_record(**bytes, part, true) || record_of(part.base) != 0) {
        continue;
      }
      extents.insert(extents.end(), part.data_extents.begin(), part.data_extents.end());
    }
  }
  std::ranges::sort(extents, {}, &Extent::vcn);
  std::uint64_t vcn = 0;
  for (const Extent& extent : extents) {
    if (extent.vcn != vcn) {
      return fail(Errc::invalid_data, "the $MFT runs have a gap");
    }
    vcn += extent.clusters;
  }
  if (extents.empty()) {
    return fail(Errc::invalid_data, "the $MFT record has no data runs");
  }
  return extents;
}

// Feeds the bytes of the $MFT, run after run, record by record into the index.
struct MftPass {
  VolumeIndex* index;
  std::size_t record_size;
  std::uint64_t total;  // records in the valid part of the $MFT
  std::uint64_t record = 0;
  RecordSegment segment;
  std::vector<std::byte> pending;  // includes the bytes of a record split across two runs

  void consume(std::span<const std::byte> bytes) {
    pending.insert(pending.end(), bytes.begin(), bytes.end());
    std::size_t used = 0;
    for (; pending.size() - used >= record_size && record < total; used += record_size) {
      // A damaged record is skipped; NTFS itself repairs such records (chkdsk).
      if (parse_record(std::span{pending}.subspan(used, record_size), segment) && segment.in_use) {
        segment.record = record;
        index->add(segment);
      }
      ++record;
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(used));
  }
};

Result<void> read_run(const INtfsVolume& volume, const Extent& extent, std::uint64_t cluster,
                      std::span<std::byte> chunk, MftPass& pass, const std::stop_token& stop) {
  const std::uint64_t length = extent.clusters * cluster;
  for (std::uint64_t done = 0; done < length && pass.record < pass.total;) {
    if (stop.stop_requested()) {
      return fail(Errc::invalid_state, "index build cancelled");
    }
    const std::span<std::byte> part =
        chunk.first(static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), length - done)));
    if (!extent.lcn) {
      std::ranges::fill(part, std::byte{0});
    } else if (Result<void> read = volume.read((*extent.lcn * cluster) + done, part); !read) {
      return read;
    }
    pass.consume(part);
    done += part.size();
  }
  return {};
}

// Where to resume reading the journal for an index read up to `cursor`, or why the journal
// cannot bring that index up to date.
tl::expected<JournalState, std::string> resume_point(const std::optional<JournalState>& cursor,
                                                     const std::optional<JournalState>& current) {
  if (!cursor) {
    return tl::unexpected<std::string>("the volume has no change journal");
  }
  if (!current) {
    return tl::unexpected<std::string>("the change journal was deleted");
  }
  if (current->id != cursor->id) {
    return tl::unexpected<std::string>("the change journal was recreated");
  }
  if (cursor->next_usn < current->first_usn) {
    return tl::unexpected<std::string>("the change journal wrapped past the records not read yet");
  }
  return JournalState{
      .id = cursor->id, .first_usn = current->first_usn, .next_usn = cursor->next_usn};
}
}  // namespace

Result<VolumeIndex> read_mft(const INtfsVolume& volume, const std::stop_token& stop) {
  const Result<VolumeGeometry> geometry = volume.geometry();
  if (!geometry) {
    return tl::unexpected(geometry.error());
  }
  if (Result<void> valid = check(*geometry); !valid) {
    return tl::unexpected(valid.error());
  }
  const Result<std::vector<Extent>> extents = mft_extents(volume, *geometry);
  if (!extents) {
    return tl::unexpected(extents.error());
  }
  VolumeIndex index{volume.root(), geometry->serial};
  MftPass pass{.index = &index,
               .record_size = geometry->bytes_per_record,
               .total = geometry->mft_valid_length / geometry->bytes_per_record};
  std::vector<std::byte> chunk(chunk_bytes);
  for (const Extent& extent : *extents) {
    if (Result<void> read =
            read_run(volume, extent, geometry->bytes_per_cluster, chunk, pass, stop);
        !read) {
      return tl::unexpected(read.error());
    }
  }
  if (pass.record < pass.total) {
    return fail(Errc::invalid_data, "the $MFT runs end before its valid length");
  }
  if (Result<void> finished = index.finish(); !finished) {
    return tl::unexpected(finished.error());
  }
  return index;
}
Result<void> enumerate_files(const INtfsVolume& volume,
                             const std::function<void(const UsnRecord&)>& sink,
                             const std::stop_token& stop) {
  std::uint64_t from = 0;
  while (!stop.stop_requested()) {
    const Result<std::vector<std::byte>> bytes = volume.enumerate(from);
    if (!bytes) {
      return tl::unexpected(bytes.error());
    }
    if (bytes->empty()) {
      return {};
    }
    const Result<UsnBuffer> buffer = parse_usn_buffer(*bytes);
    if (!buffer) {
      return tl::unexpected(buffer.error());
    }
    for (const UsnRecord& record : buffer->records) {
      sink(record);
    }
    if (buffer->records.empty() || buffer->next == from) {
      return {};
    }
    from = buffer->next;
  }
  return fail(Errc::invalid_state, "enumeration cancelled");
}

Result<IndexedVolume> IndexedVolume::build(const INtfsVolume& volume, const std::stop_token& stop) {
  const Result<std::optional<JournalState>> journal = volume.journal();
  if (!journal) {
    return tl::unexpected(journal.error());
  }
  Result<VolumeIndex> index = read_mft(volume, stop);
  if (!index) {
    return tl::unexpected(index.error());
  }
  return IndexedVolume{std::move(*index), *journal};
}

Result<RefreshOutcome> IndexedVolume::refresh(const INtfsVolume& volume,
                                              const std::stop_token& stop) {
  const auto rebuild = [&](std::string reason) -> Result<RefreshOutcome> {
    Result<IndexedVolume> built = build(volume, stop);
    if (!built) {
      return tl::unexpected(built.error());
    }
    *this = std::move(*built);
    return RefreshOutcome{.kind = RefreshKind::rebuilt, .reason = std::move(reason)};
  };
  if (index_.dead_count() * 4 > index_.node_count()) {
    return rebuild("more than a quarter of the index is dead nodes");
  }
  const Result<std::optional<JournalState>> current = volume.journal();
  if (!current) {
    return tl::unexpected(current.error());
  }
  const tl::expected<JournalState, std::string> resume = resume_point(journal_, *current);
  if (!resume) {
    return rebuild(resume.error());
  }

  JournalState cursor = *resume;
  RefreshOutcome outcome;
  while (!stop.stop_requested()) {
    const Result<std::vector<std::byte>> bytes = volume.read_journal(cursor.id, cursor.next_usn);
    if (!bytes) {
      if (bytes.error().code == Errc::not_found) {
        return rebuild("the journal records were overwritten while reading");
      }
      return tl::unexpected(bytes.error());
    }
    const Result<UsnBuffer> buffer = parse_usn_buffer(*bytes);
    if (!buffer) {
      return tl::unexpected(buffer.error());
    }
    if (buffer->records.empty()) {
      break;
    }
    if (Result<void> applied = index_.apply(buffer->records, volume); !applied) {
      return tl::unexpected(applied.error());
    }
    outcome.records += buffer->records.size();
    cursor.next_usn = static_cast<std::int64_t>(buffer->next);
    journal_ = cursor;
  }
  journal_ = cursor;
  return outcome;
}
Result<space::ScanResult> scan_tree(const VolumeIndex* index, const space::IDirectoryReader& reader,
                                    const std::filesystem::path& folder,
                                    const space::ScanOptions& options,
                                    const std::stop_token& stop) {
  if (index != nullptr && space::lower_ascii(space::utf8(index->root().root_name())) ==
                              space::lower_ascii(space::utf8(folder.root_name()))) {
    return index->to_scan_result(folder);
  }
  return space::scan(reader, folder, options, stop);
}

}  // namespace myclean::index
