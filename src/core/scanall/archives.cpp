#include "core/scanall/archives.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/content.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t zip_end_signature = 0x06054b50;
constexpr std::uint64_t zip64_locator_signature = 0x07064b50;
constexpr std::uint64_t zip64_end_signature = 0x06064b50;
constexpr std::uint64_t zip_entry_signature = 0x02014b50;
constexpr std::size_t zip_end_size = 22;
constexpr std::size_t zip64_locator_size = 20;
constexpr std::size_t zip_entry_size = 46;
constexpr std::uint64_t zip64_extra = 0x0001;
constexpr std::uint64_t zip_unset16 = 0xFFFF;
constexpr std::uint64_t zip_unset32 = 0xFFFFFFFF;

constexpr std::string_view rar4_signature{"Rar!\x1a\x07\x00", 7};
constexpr std::string_view rar5_signature{"Rar!\x1a\x07\x01\x00", 8};
constexpr std::size_t rar4_base_size = 7;
constexpr std::uint64_t rar4_main = 0x73;
constexpr std::uint64_t rar4_file = 0x74;
constexpr std::uint64_t rar4_service = 0x7A;
constexpr std::uint64_t rar4_end = 0x7B;
constexpr std::uint64_t rar4_encrypted_headers = 0x0080;
constexpr std::uint64_t rar4_folder_mask = 0x00E0;
constexpr std::uint64_t rar4_large = 0x0100;
constexpr std::uint64_t rar4_long_block = 0x8000;
constexpr std::uint64_t rar5_file = 2;
constexpr std::uint64_t rar5_encryption = 4;
constexpr std::uint64_t rar5_end = 5;
constexpr std::uint64_t rar5_has_extra = 0x0001;
constexpr std::uint64_t rar5_has_data = 0x0002;
constexpr std::uint64_t rar5_folder = 0x0001;
constexpr std::uint64_t rar5_has_time = 0x0002;
constexpr std::uint64_t rar5_has_crc = 0x0004;
constexpr std::uint64_t rar5_unknown_size = 0x0008;
constexpr std::size_t rar5_max_header = std::size_t{2} << 20U;  // the format's own limit
// The longest RAR5 size prefix: the CRC32 and a 10-byte variable-length integer.
constexpr std::size_t rar5_prefix = 14;
// Read for each RAR block first: a file header and its name usually fit.
constexpr std::size_t rar_block_read = 512;

constexpr std::string_view malformed_zip = "malformed zip central directory";
constexpr std::string_view malformed_rar = "malformed RAR header";

// Reads little-endian fields of `bytes` in order. Once a read runs past the end, every read
// returns nothing and ok() is false.
class Fields {
 public:
  explicit Fields(std::string_view bytes, std::size_t at = 0) : bytes_(bytes), at_(at) {}

  std::uint64_t fixed(std::size_t size) {
    if (!has(size)) {
      return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t index = size; index-- > 0;) {
      value = (value << 8U) | static_cast<std::uint8_t>(bytes_[at_ + index]);
    }
    at_ += size;
    return value;
  }

  // RAR5's variable-length integer: 7 bits per byte, low first, the high bit set on all but
  // the last byte; at most 10 bytes.
  std::uint64_t vint() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
      if (!has(1)) {
        return 0;
      }
      const auto byte = static_cast<std::uint8_t>(bytes_[at_++]);
      value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0) {
        return value;
      }
    }
    ok_ = false;
    return 0;
  }

  std::string_view take(std::uint64_t count) {
    if (!has(count)) {
      return {};
    }
    const std::string_view part = bytes_.substr(at_, static_cast<std::size_t>(count));
    at_ += static_cast<std::size_t>(count);
    return part;
  }

  void skip(std::uint64_t count) { static_cast<void>(take(count)); }

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] std::size_t position() const { return at_; }

 private:
  bool has(std::uint64_t count) {
    ok_ = ok_ && at_ <= bytes_.size() && bytes_.size() - at_ >= count;
    return ok_;
  }

  std::string_view bytes_;
  std::size_t at_;
  bool ok_ = true;
};

std::uint64_t field(std::string_view bytes, std::size_t at, std::size_t size) {
  return Fields(bytes, at).fixed(size);
}

// The unpacked size in a ZIP64 extended information extra field; it comes first there when the
// entry's 32-bit unpacked size is unset.
std::optional<std::uint64_t> zip64_size(std::string_view extra) {
  Fields fields(extra);
  while (fields.ok() && fields.position() < extra.size()) {
    const std::uint64_t id = fields.fixed(2);
    const std::string_view data = fields.take(fields.fixed(2));
    if (fields.ok() && id == zip64_extra && data.size() >= 8) {
      return field(data, 0, 8);
    }
  }
  return std::nullopt;
}

// The end records once the end of central directory record is found at `at` in `tail`.
Result<ZipEnd> zip_end_at(std::string_view tail, std::size_t at, std::uint64_t tail_offset) {
  const std::uint64_t disk = field(tail, at + 4, 2);
  const std::uint64_t directory_disk = field(tail, at + 6, 2);
  const std::uint64_t entries = field(tail, at + 10, 2);
  const std::uint64_t size = field(tail, at + 12, 4);
  const std::uint64_t offset = field(tail, at + 16, 4);
  const std::uint64_t end = tail_offset + at;
  if (at >= zip64_locator_size &&
      field(tail, at - zip64_locator_size, 4) == zip64_locator_signature) {
    const std::uint64_t record = field(tail, at - zip64_locator_size + 8, 8);
    if (record > end - zip64_locator_size || end - zip64_locator_size - record < zip64_end_size) {
      return fail(Errc::invalid_data, "ZIP64 end record past the end of the zip");
    }
    return ZipEnd{.zip64_record = record};
  }
  if (disk == zip_unset16 || directory_disk == zip_unset16 || entries == zip_unset16 ||
      size == zip_unset32 || offset == zip_unset32) {
    return fail(Errc::invalid_data, "ZIP64 zip without its ZIP64 locator");
  }
  if (disk != directory_disk) {
    return fail(Errc::invalid_data, "the central directory starts in another part");
  }
  if (offset > end || end - offset < size) {
    return fail(Errc::invalid_data, "central directory past its end record");
  }
  return ZipEnd{.directory = ZipDirectory{.offset = offset, .size = size}};
}

Result<RarBlock> rar4_file_block(std::string_view header, std::uint64_t type, std::uint64_t flags) {
  Fields fields(header, rar4_base_size);
  std::uint64_t packed = fields.fixed(4);
  std::uint64_t unpacked = fields.fixed(4);
  fields.skip(1 + 4 + 4 + 1 + 1);  // host OS, CRC32, time, version, method
  const std::uint64_t name_size = fields.fixed(2);
  fields.skip(4);  // attributes
  if ((flags & rar4_large) != 0) {
    packed |= fields.fixed(4) << 32U;
    unpacked |= fields.fixed(4) << 32U;
  }
  std::string_view name = fields.take(name_size);
  if (!fields.ok()) {
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  RarBlock block{.size = header.size() + packed};
  if (type == rar4_file && (flags & rar4_folder_mask) != rar4_folder_mask) {
    // With the Unicode flag, the OEM name is followed by a NUL and an encoded Unicode name.
    name = name.substr(0, name.find('\0'));
    block.member = ArchiveMember{.name = std::string(name), .size = unpacked};
  }
  return block;
}

Result<RarBlock> parse_rar4_block(std::string_view bytes) {
  if (bytes.size() < rar4_base_size) {
    return RarBlock{.needed = rar4_base_size};
  }
  Fields head(bytes, 2);
  const std::uint64_t type = head.fixed(1);
  const std::uint64_t flags = head.fixed(2);
  const auto size = static_cast<std::size_t>(head.fixed(2));
  if (size < rar4_base_size) {
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  if (bytes.size() < size) {
    return RarBlock{.needed = size};
  }
  const std::string_view header = bytes.substr(0, size);
  if (type == rar4_main && (flags & rar4_encrypted_headers) != 0) {
    return fail(Errc::refused, "encrypted RAR headers");
  }
  if (type == rar4_file || type == rar4_service) {
    return rar4_file_block(header, type, flags);
  }
  RarBlock block{.size = size, .end = type == rar4_end};
  if ((flags & rar4_long_block) != 0) {
    Fields data(header, rar4_base_size);
    block.size += data.fixed(4);
    if (!data.ok()) {
      return fail(Errc::invalid_data, std::string(malformed_rar));
    }
  }
  return block;
}

// The member of a RAR5 file header, read from its type-specific fields; nullopt for a folder
// or a file whose size is not known.
Result<std::optional<ArchiveMember>> rar5_member(Fields& fields) {
  const std::uint64_t file_flags = fields.vint();
  const std::uint64_t unpacked = fields.vint();
  fields.vint();  // attributes
  if ((file_flags & rar5_has_time) != 0) {
    fields.skip(4);
  }
  if ((file_flags & rar5_has_crc) != 0) {
    fields.skip(4);
  }
  fields.vint();  // compression
  fields.vint();  // host OS
  const std::string_view name = fields.take(fields.vint());
  if (!fields.ok()) {
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  if ((file_flags & (rar5_folder | rar5_unknown_size)) != 0) {
    return std::optional<ArchiveMember>{};
  }
  return std::optional<ArchiveMember>{ArchiveMember{.name = std::string(name), .size = unpacked}};
}

Result<RarBlock> parse_rar5_block(std::string_view bytes) {
  Fields prefix(bytes, 4);  // after the CRC32
  const std::uint64_t header_size = prefix.vint();
  if (!prefix.ok()) {
    if (bytes.size() < rar5_prefix) {
      return RarBlock{.needed = rar5_prefix};
    }
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  if (header_size == 0 || header_size > rar5_max_header) {
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  const std::size_t total = prefix.position() + static_cast<std::size_t>(header_size);
  if (bytes.size() < total) {
    return RarBlock{.needed = total};
  }
  Fields fields(bytes.substr(0, total), prefix.position());
  const std::uint64_t type = fields.vint();
  const std::uint64_t flags = fields.vint();
  if ((flags & rar5_has_extra) != 0) {
    fields.vint();
  }
  const std::uint64_t data = (flags & rar5_has_data) != 0 ? fields.vint() : 0;
  if (type == rar5_encryption) {
    return fail(Errc::refused, "encrypted RAR headers");
  }
  RarBlock block{.size = total + data, .end = type == rar5_end};
  if (type == rar5_file) {
    Result<std::optional<ArchiveMember>> member = rar5_member(fields);
    if (!member) {
      return tl::unexpected(member.error());
    }
    block.member = *std::move(member);
  }
  if (!fields.ok()) {
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  return block;
}

// --- Volume names ---

std::wstring lower(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return result;
}

// "x.z01" with `mark` ".z" and 2 to 3 digits: {"x", 1}.
std::optional<std::pair<std::wstring_view, int>> numbered(std::wstring_view name,
                                                          std::wstring_view mark,
                                                          std::size_t min_digits,
                                                          std::size_t max_digits) {
  const std::size_t at = name.rfind(mark);
  if (at == std::wstring_view::npos || at == 0) {
    return std::nullopt;
  }
  const std::wstring_view digits = name.substr(at + mark.size());
  if (digits.size() < min_digits || digits.size() > max_digits ||
      !std::ranges::all_of(digits, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
    return std::nullopt;
  }
  int number = 0;
  for (const wchar_t c : digits) {
    number = (number * 10) + (c - L'0');
  }
  return std::pair{name.substr(0, at), number};
}

struct VolumeName {
  std::wstring key;  // the set: its stem and naming scheme
  ArchiveFormat format = ArchiveFormat::zip;
  int order = 0;       // in the set
  bool first = false;  // X.zip, X.rar, X.part1.rar: the volume a set starts with
};

std::optional<VolumeName> volume_name(std::wstring_view name) {
  if (name.ends_with(L".zip")) {
    return VolumeName{.key = std::wstring(name.substr(0, name.size() - 4)) + L"|zip",
                      .first = true};
  }
  if (const auto part = numbered(name, L".z", 2, 3)) {
    return VolumeName{.key = std::wstring(part->first) + L"|zip", .order = part->second};
  }
  if (name.ends_with(L".rar")) {
    const std::wstring_view stem = name.substr(0, name.size() - 4);
    if (const auto part = numbered(stem, L".part", 1, 4)) {
      return VolumeName{.key = std::wstring(part->first) + L"|part",
                        .format = ArchiveFormat::rar,
                        .order = part->second,
                        .first = part->second <= 1};
    }
    return VolumeName{
        .key = std::wstring(stem) + L"|rar", .format = ArchiveFormat::rar, .first = true};
  }
  if (const auto part = numbered(name, L".r", 2, 3)) {
    return VolumeName{.key = std::wstring(part->first) + L"|rar",
                      .format = ArchiveFormat::rar,
                      .order = part->second + 1};
  }
  return std::nullopt;
}

// --- Listing within the budgets ---

// Reads through `reader` while the budgets last and `stop` is not requested.
class BudgetReader {
 public:
  BudgetReader(const space::IContentReader& reader, const ArchiveOptions& options,
               std::stop_token stop)
      : reader_(&reader), options_(&options), stop_(std::move(stop)) {}

  void start_set() {
    set_read_ = 0;
    exhausted_ = false;
  }

  Result<std::string> read(const fs::path& file, std::uint64_t offset, std::size_t length) {
    if (stop_.stop_requested()) {
      exhausted_ = true;
      return fail(Errc::refused, "stopped");
    }
    if (read_ + length > options_->budget.count() ||
        set_read_ + length > options_->per_archive.count()) {
      exhausted_ = true;
      return fail(Errc::refused, "the read budget is spent");
    }
    Result<std::string> bytes = reader_->read(file, offset, length);
    if (bytes) {
      read_ += bytes->size();
      set_read_ += bytes->size();
    }
    return bytes;
  }

  // The bytes at `offset` that must all be there.
  Result<std::string> read_all(const fs::path& file, std::uint64_t offset, std::size_t length) {
    Result<std::string> bytes = read(file, offset, length);
    if (bytes && bytes->size() != length) {
      return fail(Errc::invalid_data, "truncated archive");
    }
    return bytes;
  }

  [[nodiscard]] bool exhausted() const { return exhausted_; }
  [[nodiscard]] Bytes total() const { return Bytes{read_}; }

 private:
  const space::IContentReader* reader_;
  const ArchiveOptions* options_;
  std::stop_token stop_;
  std::uint64_t read_ = 0;
  std::uint64_t set_read_ = 0;
  bool exhausted_ = false;
};

Result<std::vector<ArchiveMember>> zip_members(BudgetReader& reader, const ArchiveVolume& zip) {
  const std::uint64_t size = zip.size.count();
  const std::uint64_t tail_offset = size - std::min<std::uint64_t>(size, zip_tail_size);
  const Result<std::string> tail =
      reader.read_all(zip.path, tail_offset, static_cast<std::size_t>(size - tail_offset));
  if (!tail) {
    return tl::unexpected(tail.error());
  }
  const Result<ZipEnd> end = parse_zip_end(*tail, tail_offset);
  if (!end) {
    return tl::unexpected(end.error());
  }
  Result<ZipDirectory> directory = end->directory ? Result<ZipDirectory>(*end->directory)
                                                  : fail(Errc::invalid_data, "no directory");
  if (end->zip64_record) {
    const Result<std::string> record =
        reader.read_all(zip.path, *end->zip64_record, zip64_end_size);
    directory = record ? parse_zip64_end(*record) : tl::unexpected(record.error());
  }
  if (!directory) {
    return tl::unexpected(directory.error());
  }
  if (directory->offset > size || size - directory->offset < directory->size) {
    return fail(Errc::invalid_data, "central directory past the end of the zip");
  }
  const Result<std::string> bytes =
      reader.read_all(zip.path, directory->offset, static_cast<std::size_t>(directory->size));
  if (!bytes) {
    return tl::unexpected(bytes.error());
  }
  return parse_zip_directory(*bytes);
}

// The next block of a RAR volume at `offset`, read again when its header is longer.
Result<RarBlock> next_rar_block(BudgetReader& reader, const ArchiveVolume& volume,
                                std::uint64_t offset, RarVersion version) {
  const std::uint64_t left = volume.size.count() - offset;
  const Result<std::string> bytes = reader.read(
      volume.path, offset, static_cast<std::size_t>(std::min<std::uint64_t>(left, rar_block_read)));
  if (!bytes) {
    return tl::unexpected(bytes.error());
  }
  Result<RarBlock> block = parse_rar_block(*bytes, version);
  if (!block || block->needed == 0) {
    return block;
  }
  if (block->needed > left || block->needed <= bytes->size()) {
    return fail(Errc::invalid_data, "truncated archive");
  }
  const Result<std::string> longer = reader.read_all(volume.path, offset, block->needed);
  if (!longer) {
    return tl::unexpected(longer.error());
  }
  block = parse_rar_block(*longer, version);
  if (block && block->needed != 0) {
    return fail(Errc::invalid_data, std::string(malformed_rar));
  }
  return block;
}

// Adds the members of one RAR volume whose names `seen` does not hold yet: a file split across
// volumes has a header in each.
Result<void> rar_members(BudgetReader& reader, const ArchiveVolume& volume,
                         std::set<std::string>& seen, std::vector<ArchiveMember>& members) {
  const Result<std::string> head = reader.read(volume.path, 0, rar5_signature.size());
  if (!head) {
    return tl::unexpected(head.error());
  }
  const std::optional<RarVersion> version = rar_version(*head);
  if (!version) {
    return fail(Errc::invalid_data, "not a RAR archive");
  }
  for (std::uint64_t offset = rar_signature_size(*version); offset < volume.size.count();) {
    Result<RarBlock> block = next_rar_block(reader, volume, offset, *version);
    if (!block) {
      return tl::unexpected(block.error());
    }
    if (const std::optional<ArchiveMember>& member = block->member;
        member && seen.insert(member->name).second) {
      members.push_back(*member);
    }
    if (block->end || block->size >= volume.size.count() - offset) {
      break;
    }
    offset += block->size;
  }
  return {};
}

Result<std::vector<ArchiveMember>> list_members(BudgetReader& reader, const ArchiveSet& set) {
  if (set.format == ArchiveFormat::zip) {
    return zip_members(reader, set.volumes.front());
  }
  std::set<std::string> seen;
  std::vector<ArchiveMember> members;
  for (const ArchiveVolume& volume : set.volumes) {
    if (Result<void> listed = rar_members(reader, volume, seen, members); !listed) {
      return tl::unexpected(listed.error());
    }
  }
  return members;
}

// --- Matching ---

// A file by its lower-case name and its size.
using Key = std::pair<std::string, std::uint64_t>;

Key member_key(const ArchiveMember& member) {
  const std::size_t slash = member.name.find_last_of("/\\");
  const std::string_view leaf = slash == std::string::npos
                                    ? std::string_view(member.name)
                                    : std::string_view(member.name).substr(slash + 1);
  return {space::lower_ascii(leaf), member.size};
}

bool comparable(const space::FileItem& file) {
  return !file.link && !file.placeholder;
}

bool backup_path(const fs::path& path) {
  return std::ranges::any_of(path.relative_path(), [](const fs::path& part) {
    return clean::wildcard_match(part.native(), L"*backup*");
  });
}

struct Listed {
  ArchiveSet set;
  std::vector<ArchiveMember> members;  // of min_member or more
};

// The files of `trees` with the name and size of a member of `listed`, by key.
std::map<Key, std::vector<fs::path>> unpacked_copies(std::span<const space::ScanResult> trees,
                                                     std::span<const Listed> listed,
                                                     Bytes min_member) {
  std::map<Key, std::vector<fs::path>> copies;
  for (const Listed& archive : listed) {
    for (const ArchiveMember& member : archive.members) {
      copies.try_emplace(member_key(member));
    }
  }
  for (const space::ScanResult& tree : trees) {
    for (const space::Folder& folder : tree.folders) {
      for (const space::FileItem& file : folder.files) {
        if (!comparable(file) || file.size < min_member) {
          continue;
        }
        const auto found =
            copies.find({space::lower_ascii(space::utf8(file.name)), file.size.count()});
        if (found != copies.end()) {
          found->second.push_back(space::path_of(folder, file));
        }
      }
    }
  }
  return copies;
}

// The deepest folder holding both.
fs::path common_folder(const fs::path& left, const fs::path& right) {
  fs::path common;
  for (auto one = left.begin(), two = right.begin();
       one != left.end() && two != right.end() &&
       space::lower_ascii(space::utf8(*one)) == space::lower_ascii(space::utf8(*two));
       ++one, ++two) {
    common /= *one;
  }
  return common;
}

struct Match {
  Bytes total;           // of the members compared
  Bytes matched;         // of those with an unpacked copy
  Bytes outside_backup;  // of those with a copy outside any backup-like folder
  // The folder of each chosen copy, with the member's size.
  std::vector<std::pair<fs::path, Bytes>> places;
};

// The copy of a member to name: outside a backup-like folder when there is one, and never one
// of the set's own volumes; nullopt when there is none.
std::optional<fs::path> chosen_copy(const ArchiveSet& set, std::span<const fs::path> copies) {
  std::optional<fs::path> chosen;
  for (const fs::path& copy : copies) {
    const bool own = std::ranges::any_of(set.volumes, [&copy](const ArchiveVolume& volume) {
      return placement::same_path(volume.path, copy);
    });
    if (!own && (!chosen || (backup_path(*chosen) && !backup_path(copy)))) {
      chosen = copy;
    }
  }
  return chosen;
}

Match match_members(const Listed& archive, const std::map<Key, std::vector<fs::path>>& copies) {
  Match result;
  for (const ArchiveMember& member : archive.members) {
    const Bytes size{member.size};
    result.total += size;
    const auto found = copies.find(member_key(member));
    if (found == copies.end()) {
      continue;
    }
    if (const std::optional<fs::path> chosen = chosen_copy(archive.set, found->second)) {
      const bool backup = backup_path(*chosen);
      result.matched += size;
      result.outside_backup += backup ? Bytes{} : size;
      result.places.emplace_back(chosen->parent_path(), size);
    }
  }
  return result;
}

// The folder holding the unpacked copies on the drive that holds most of their bytes, and
// whether it holds all of them.
std::pair<fs::path, bool> unpacked_folder(const Match& found) {
  std::map<std::wstring, Bytes> by_drive;
  for (const auto& [folder, size] : found.places) {
    by_drive[placement::drive_root(folder)] += size;
  }
  const auto drive = std::ranges::max_element(
      by_drive, [](const auto& left, const auto& right) { return left.second < right.second; });
  if (drive == by_drive.end()) {
    return {};
  }
  std::optional<fs::path> common;
  for (const auto& [folder, size] : found.places) {
    if (placement::drive_root(folder) == drive->first) {
      common = common ? common_folder(*common, folder) : folder;
    }
  }
  return {common.value_or(fs::path{}), drive->second == found.matched};
}

std::optional<Finding> extracted_finding(const Listed& archive, const Match& found,
                                         const ArchiveOptions& options) {
  const auto total = static_cast<double>(found.total.count());
  if (found.total.count() == 0 ||
      static_cast<double>(found.matched.count()) < options.min_share * total) {
    return std::nullopt;
  }
  const auto [folder, all] = unpacked_folder(found);
  std::string reason = std::format(
      "{:.0f} % of the {} in its {} members of {} or more exist unpacked (same name and size) "
      "{}in {}; a download or purchase of the owner's: delete it only when it can be downloaded "
      "again",
      std::floor(static_cast<double>(found.matched.count()) * 100.0 / total),
      size_text(found.total), archive.members.size(), size_text(options.min_member),
      all ? "" : "mostly ", space::utf8(folder));
  if (static_cast<double>(found.outside_backup.count()) < options.min_share * total) {
    reason +=
        "; the unpacked copy is only inside a backup, which deleting the archive would "
        "leave as the only copy";
  }
  const fs::path& first = archive.set.volumes.front().path;
  Finding finding{
      .group = ActionGroup::manual_review,
      .source = "archives",
      .drive = fs::path{placement::drive_root(first)},
      .path = first,
      .bytes = archive.set.size_on_disk,
      .confidence = 0.7,
      .reason = std::move(reason),
      .plan = Plan{.kind = PlanKind::review, .params = {{"extracted", space::utf8(folder)}}}};
  for (const ArchiveVolume& volume : archive.set.volumes) {
    finding.items.push_back(Item{.path = volume.path, .size = volume.size_on_disk});
  }
  return finding;
}

std::vector<ArchiveSet> large_sets(std::span<const space::ScanResult> trees, Bytes min_size) {
  std::vector<ArchiveSet> sets;
  for (const space::ScanResult& tree : trees) {
    for (const space::Folder& folder : tree.folders) {
      for (ArchiveSet& set : archive_sets(folder)) {
        if (set.size >= min_size && !system_location(folder.path)) {
          sets.push_back(std::move(set));
        }
      }
    }
  }
  std::ranges::stable_sort(
      sets, [](const ArchiveSet& left, const ArchiveSet& right) { return left.size > right.size; });
  return sets;
}

}  // namespace

Result<ZipEnd> parse_zip_end(std::string_view tail, std::uint64_t tail_offset) {
  if (tail.size() >= zip_end_size) {
    for (std::size_t at = tail.size() - zip_end_size + 1; at-- > 0;) {
      if (field(tail, at, 4) == zip_end_signature &&
          at + zip_end_size + field(tail, at + 20, 2) <= tail.size()) {
        return zip_end_at(tail, at, tail_offset);
      }
    }
  }
  return fail(Errc::invalid_data, "no zip end of central directory record");
}

Result<ZipDirectory> parse_zip64_end(std::string_view record) {
  Fields fields(record);
  const std::uint64_t signature = fields.fixed(4);
  fields.skip(8 + 2 + 2);  // record size, versions
  const std::uint64_t disk = fields.fixed(4);
  const std::uint64_t directory_disk = fields.fixed(4);
  fields.skip(8 + 8);  // entries on this disk, in all
  const std::uint64_t size = fields.fixed(8);
  const std::uint64_t offset = fields.fixed(8);
  if (!fields.ok() || signature != zip64_end_signature) {
    return fail(Errc::invalid_data, "malformed ZIP64 end record");
  }
  if (disk != directory_disk) {
    return fail(Errc::invalid_data, "the central directory starts in another part");
  }
  return ZipDirectory{.offset = offset, .size = size};
}

Result<std::vector<ArchiveMember>> parse_zip_directory(std::string_view directory) {
  std::vector<ArchiveMember> members;
  Fields fields(directory);
  while (fields.position() < directory.size()) {
    const std::uint64_t signature = fields.fixed(4);
    fields.skip(20);  // versions, flags, method, time, date, CRC32, packed size
    std::uint64_t size = fields.fixed(4);
    const std::uint64_t name_size = fields.fixed(2);
    const std::uint64_t extra_size = fields.fixed(2);
    const std::uint64_t comment_size = fields.fixed(2);
    fields.skip(zip_entry_size - 34);  // disk, attributes, local header offset
    const std::string_view name = fields.take(name_size);
    const std::string_view extra = fields.take(extra_size);
    fields.skip(comment_size);
    if (!fields.ok() || signature != zip_entry_signature) {
      return fail(Errc::invalid_data, std::string(malformed_zip));
    }
    if (size == zip_unset32) {
      const std::optional<std::uint64_t> large = zip64_size(extra);
      if (!large) {
        return fail(Errc::invalid_data, std::string(malformed_zip));
      }
      size = *large;
    }
    if (!name.empty() && name.back() != '/' && name.back() != '\\') {
      members.push_back(ArchiveMember{.name = std::string(name), .size = size});
    }
  }
  return members;
}

std::optional<RarVersion> rar_version(std::string_view head) {
  if (head.starts_with(rar5_signature)) {
    return RarVersion::rar5;
  }
  if (head.starts_with(rar4_signature)) {
    return RarVersion::rar4;
  }
  return std::nullopt;
}

std::size_t rar_signature_size(RarVersion version) {
  return version == RarVersion::rar5 ? rar5_signature.size() : rar4_signature.size();
}

Result<RarBlock> parse_rar_block(std::string_view bytes, RarVersion version) {
  return version == RarVersion::rar5 ? parse_rar5_block(bytes) : parse_rar4_block(bytes);
}

std::vector<ArchiveSet> archive_sets(const space::Folder& folder) {
  struct Building {
    ArchiveSet set;
    std::vector<std::pair<int, ArchiveVolume>> volumes;
    bool first = false;
  };
  std::map<std::wstring, Building> building;
  for (const space::FileItem& file : folder.files) {
    if (!comparable(file) || file.extra_link) {
      continue;
    }
    const std::optional<VolumeName> name = volume_name(lower(file.name.native()));
    if (!name) {
      continue;
    }
    Building& entry = building[name->key];
    entry.set.format = name->format;
    entry.set.size += file.size;
    entry.set.size_on_disk += file.size_on_disk;
    entry.first = entry.first || name->first;
    entry.volumes.emplace_back(name->order, ArchiveVolume{.path = space::path_of(folder, file),
                                                          .size = file.size,
                                                          .size_on_disk = file.size_on_disk});
  }
  std::vector<ArchiveSet> sets;
  for (auto& [key, entry] : building) {
    if (entry.set.format == ArchiveFormat::zip && !entry.first) {
      continue;  // a split zip's directory is in X.zip
    }
    std::ranges::stable_sort(entry.volumes, {}, &std::pair<int, ArchiveVolume>::first);
    for (auto& [order, volume] : entry.volumes) {
      entry.set.volumes.push_back(std::move(volume));
    }
    sets.push_back(std::move(entry.set));
  }
  return sets;
}

ArchiveRun find_extracted_archives(std::span<const space::ScanResult> trees,
                                   const space::IContentReader& reader,
                                   const ArchiveOptions& options, const std::stop_token& stop) {
  ArchiveRun run;
  BudgetReader budget(reader, options, stop);
  std::vector<Listed> listed;
  for (ArchiveSet& set : large_sets(trees, options.min_size)) {
    if (stop.stop_requested()) {
      run.cancelled = true;
      break;
    }
    budget.start_set();
    Result<std::vector<ArchiveMember>> members = list_members(budget, set);
    if (!members) {
      ++(budget.exhausted() ? run.skipped : run.unreadable);
      continue;
    }
    ++run.listed;
    std::erase_if(*members, [&options](const ArchiveMember& member) {
      return Bytes{member.size} < options.min_member;
    });
    listed.push_back(Listed{.set = std::move(set), .members = *std::move(members)});
  }
  run.cancelled = run.cancelled || stop.stop_requested();
  run.read = budget.total();
  const std::map<Key, std::vector<fs::path>> copies =
      unpacked_copies(trees, listed, options.min_member);
  for (const Listed& archive : listed) {
    if (std::optional<Finding> finding =
            extracted_finding(archive, match_members(archive, copies), options)) {
      run.findings.push_back(*std::move(finding));
    }
  }
  return run;
}

}  // namespace myclean::scanall
