#include "core/scanall/archives.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/content.hpp"
#include "core/space/scanner.hpp"
#include "scanall/scanall_fakes.hpp"
#include "space/space_fakes.hpp"

namespace myclean::scanall {
namespace {

namespace fs = std::filesystem;
using namespace myclean::literals;
using space::testing::FakeDirectoryReader;
using space::testing::scan_ok;

const WallTime older = std::chrono::sys_days{std::chrono::year{2025} / 3 / 1};

using Members = std::vector<std::pair<std::string, std::uint64_t>>;

// --- Crafted archives ---

std::string le(std::uint64_t value, std::size_t size) {
  std::string bytes;
  for (std::size_t index = 0; index < size; ++index) {
    bytes.push_back(static_cast<char>((value >> (8 * index)) & 0xFFU));
  }
  return bytes;
}

std::string zip_entry(const std::string& name, std::uint64_t size, bool zip64) {
  const std::string extra = zip64 ? le(0x0001, 2) + le(8, 2) + le(size, 8) : "";
  return le(0x02014b50, 4) + std::string(20, '\0') + le(zip64 ? 0xFFFFFFFF : size, 4) +
         le(name.size(), 2) + le(extra.size(), 2) + le(0, 2) + std::string(12, '\0') + name + extra;
}

// A zip whose local headers and data are `data` filler bytes, then its central directory and
// end records (ZIP64 ones with `zip64`), then `comment`.
std::string zip_file(const Members& members, bool zip64 = false, const std::string& comment = "") {
  std::string file(100, 'd');
  const std::size_t directory_offset = file.size();
  for (const auto& [name, size] : members) {
    file += zip_entry(name, size, zip64);
  }
  const std::size_t directory_size = file.size() - directory_offset;
  if (zip64) {
    const std::size_t record = file.size();
    file += le(0x06064b50, 4) + le(44, 8) + le(45, 2) + le(45, 2) + le(0, 4) + le(0, 4) +
            le(members.size(), 8) + le(members.size(), 8) + le(directory_size, 8) +
            le(directory_offset, 8);
    file += le(0x07064b50, 4) + le(0, 4) + le(record, 8) + le(1, 4);
    file += le(0x06054b50, 4) + le(0xFFFF, 2) + le(0xFFFF, 2) + le(0xFFFF, 2) + le(0xFFFF, 2) +
            le(0xFFFFFFFF, 4) + le(0xFFFFFFFF, 4);
  } else {
    file += le(0x06054b50, 4) + le(0, 2) + le(0, 2) + le(members.size(), 2) +
            le(members.size(), 2) + le(directory_size, 4) + le(directory_offset, 4);
  }
  return file + le(comment.size(), 2) + comment;
}

Result<std::vector<ArchiveMember>> zip_listing(const std::string& file) {
  const std::size_t tail_offset = file.size() > zip_tail_size ? file.size() - zip_tail_size : 0;
  const Result<ZipEnd> end = parse_zip_end(std::string_view(file).substr(tail_offset), tail_offset);
  if (!end) {
    return tl::unexpected(end.error());
  }
  ZipDirectory directory;
  if (end->zip64_record) {
    const Result<ZipDirectory> record = parse_zip64_end(std::string_view(file).substr(
        static_cast<std::size_t>(*end->zip64_record), zip64_end_size));
    if (!record) {
      return tl::unexpected(record.error());
    }
    directory = *record;
  } else {
    directory = *end->directory;
  }
  return parse_zip_directory(std::string_view(file).substr(
      static_cast<std::size_t>(directory.offset), static_cast<std::size_t>(directory.size)));
}

constexpr std::string_view rar4_signature{"Rar!\x1a\x07\x00", 7};
constexpr std::string_view rar5_signature{"Rar!\x1a\x07\x01\x00", 8};

std::string rar4_block(std::uint64_t type, std::uint64_t flags, const std::string& body,
                       std::size_t data = 0) {
  return le(0, 2) + le(type, 1) + le(flags, 2) + le(7 + body.size(), 2) + body +
         std::string(data, 'x');
}

// A RAR4 file header (LONG_BLOCK; LHD_LARGE when the size needs 64 bits) and its packed data.
std::string rar4_file(const std::string& name, std::uint64_t size, std::uint64_t flags = 0,
                      std::size_t packed = 10) {
  const bool large = size > 0xFFFFFFFFU;
  std::string body = le(packed, 4) + le(size & 0xFFFFFFFFU, 4) + le(2, 1) + le(0, 4) + le(0, 4) +
                     le(29, 1) + le(0x33, 1) + le(name.size(), 2) + le(0x20, 4);
  if (large) {
    body += le(0, 4) + le(size >> 32U, 4);
  }
  return rar4_block(0x74, 0x8000 | flags | (large ? 0x0100 : 0), body + name, packed);
}

std::string rar4_archive(const std::vector<std::string>& blocks, std::uint64_t main_flags = 0) {
  std::string file = std::string(rar4_signature) + rar4_block(0x73, main_flags, le(0, 6));
  for (const std::string& block : blocks) {
    file += block;
  }
  return file + rar4_block(0x7B, 0x4000, "");
}

std::string vint(std::uint64_t value) {
  std::string bytes;
  for (; value >= 0x80U; value >>= 7U) {
    bytes.push_back(static_cast<char>((value & 0x7FU) | 0x80U));
  }
  bytes.push_back(static_cast<char>(value));
  return bytes;
}

std::string rar5_header(std::uint64_t type, const std::string& fields, std::size_t data = 0,
                        std::uint64_t flags = 0) {
  std::string body = vint(type) + vint(flags | (data > 0 ? 0x0002 : 0));
  if (data > 0) {
    body += vint(data);
  }
  body += fields;
  return le(0, 4) + vint(body.size()) + body + std::string(data, 'x');
}

std::string rar5_file(const std::string& name, std::uint64_t size, std::uint64_t file_flags = 0) {
  std::string fields = vint(file_flags | 0x0004) + vint(size) + vint(0x20) + le(0, 4);
  if ((file_flags & 0x0002) != 0) {
    fields += le(0, 4);
  }
  return rar5_header(2, fields + vint(0) + vint(0) + vint(name.size()) + name, 10);
}

std::string rar5_archive(const std::vector<std::string>& blocks) {
  std::string file = std::string(rar5_signature) + rar5_header(1, vint(0));
  for (const std::string& block : blocks) {
    file += block;
  }
  return file + rar5_header(5, vint(0));
}

// Every member of a RAR file, parsed block by block from the whole of it.
Result<std::vector<ArchiveMember>> rar_listing(const std::string& file) {
  const std::optional<RarVersion> version = rar_version(file);
  if (!version) {
    return fail(Errc::invalid_data, "not a RAR archive");
  }
  std::vector<ArchiveMember> members;
  for (std::size_t offset = rar_signature_size(*version); offset < file.size();) {
    const Result<RarBlock> block = parse_rar_block(std::string_view(file).substr(offset), *version);
    if (!block) {
      return tl::unexpected(block.error());
    }
    if (block->needed != 0) {
      return fail(Errc::invalid_data, "truncated");
    }
    if (block->member) {
      members.push_back(*block->member);
    }
    if (block->end) {
      break;
    }
    offset += static_cast<std::size_t>(block->size);
  }
  return members;
}

Members pairs(const Result<std::vector<ArchiveMember>>& members) {
  Members result;
  for (const ArchiveMember& member : members.value()) {
    result.emplace_back(member.name, member.size);
  }
  return result;
}

// --- Parsing ---

TEST(ZipParsing, ListsTheCentralDirectoryWithoutFolders) {
  const std::string file =
      zip_file({{"Pack/", 0}, {"Pack/a.bin", 3000}, {"readme.txt", 12}}, false, "a comment");

  const Result<std::vector<ArchiveMember>> members = zip_listing(file);

  ASSERT_TRUE(members.has_value()) << members.error().message;
  EXPECT_EQ(pairs(members), (Members{{"Pack/a.bin", 3000}, {"readme.txt", 12}}));
}

TEST(ZipParsing, ReadsZip64Sizes) {
  const std::uint64_t huge = std::uint64_t{5} << 30U;
  const std::string file = zip_file({{"big.uasset", huge}, {"small.bin", 7}}, true);

  const Result<std::vector<ArchiveMember>> members = zip_listing(file);

  ASSERT_TRUE(members.has_value()) << members.error().message;
  EXPECT_EQ(pairs(members), (Members{{"big.uasset", huge}, {"small.bin", 7}}));
}

TEST(ZipParsing, FindsTheEndRecordInTheLastBytes) {
  const std::string file = std::string(200'000, 'p') + zip_file({{"a.bin", 5}});
  const std::size_t tail_offset = file.size() - zip_tail_size;

  const Result<ZipEnd> end = parse_zip_end(std::string_view(file).substr(tail_offset), tail_offset);

  ASSERT_TRUE(end.has_value()) << end.error().message;
  // As recorded: the prefix is not the zip's.
  EXPECT_EQ(end->directory.value_or(ZipDirectory{}).offset, 100U);
}

TEST(ZipParsing, RefusesGarbageAndTruncatedInput) {
  EXPECT_FALSE(parse_zip_end("", 0).has_value());
  EXPECT_FALSE(parse_zip_end(std::string(5000, 'g'), 0).has_value());
  const std::string file = zip_file({{"a.bin", 5}, {"b.bin", 6}});
  // The end record names a directory that its tail no longer reaches.
  EXPECT_FALSE(parse_zip_end(std::string_view(file).substr(150), 0).has_value());
  const std::string directory = zip_entry("a.bin", 5, false) + zip_entry("b.bin", 6, false);
  EXPECT_FALSE(parse_zip_directory(directory.substr(0, directory.size() - 3)).has_value());
  EXPECT_FALSE(parse_zip_directory(std::string(100, '\x50')).has_value());
  // A 32-bit size that asks for the ZIP64 field, which is missing.
  EXPECT_FALSE(parse_zip_directory(le(0x02014b50, 4) + std::string(20, '\0') + le(0xFFFFFFFF, 4) +
                                   le(1, 2) + std::string(16, '\0') + "a")
                   .has_value());
  EXPECT_FALSE(parse_zip64_end(std::string(zip64_end_size, '\0')).has_value());
  EXPECT_FALSE(parse_zip64_end("PK").has_value());
}

TEST(ZipParsing, RefusesADirectoryThatStartsInAnotherPart) {
  std::string file = zip_file({{"a.bin", 5}});
  file[file.size() - 22 + 4] = 1;  // this is part 2; the directory starts in part 1

  EXPECT_FALSE(parse_zip_end(file, 0).has_value());
}

TEST(RarParsing, ListsRar4FilesWithLargeAndUnicodeNames) {
  const std::uint64_t huge = (std::uint64_t{6} << 30U) + 5;
  const std::string file = rar4_archive({
      rar4_file("Pack\\a.bin", 3000), rar4_file("Pack", 0, 0x00E0),  // a folder
      rar4_file(std::string("caf\xe9.bin\0\x01\x02", 11), 400, 0x0200),
      rar4_file("big.uasset", huge),
      rar4_block(0x78, 0x8000, le(20, 4) + std::string(8, '\0'), 20),  // recovery record
  });

  const Result<std::vector<ArchiveMember>> members = rar_listing(file);

  ASSERT_TRUE(members.has_value()) << members.error().message;
  EXPECT_EQ(pairs(members),
            (Members{{"Pack\\a.bin", 3000}, {"caf\xe9.bin", 400}, {"big.uasset", huge}}));
}

TEST(RarParsing, ListsRar5FilesAndSkipsServiceHeadersAndFolders) {
  const std::uint64_t huge = std::uint64_t{9} << 30U;
  const std::string file = rar5_archive({
      rar5_file("Pack/a.bin", 3000, 0x0002),
      rar5_file("Pack", 0, 0x0001),
      rar5_header(3, vint(0) + vint(0) + vint(0) + vint(0) + vint(0) + vint(3) + "CMT", 30),
      rar5_file("Pack/b.bin", 2000),
      rar5_file("streamed.bin", 0, 0x0008),  // unknown size
      rar5_file("big.uasset", huge),
  });

  const Result<std::vector<ArchiveMember>> members = rar_listing(file);

  ASSERT_TRUE(members.has_value()) << members.error().message;
  EXPECT_EQ(pairs(members),
            (Members{{"Pack/a.bin", 3000}, {"Pack/b.bin", 2000}, {"big.uasset", huge}}));
}

TEST(RarParsing, AsksForMoreBytesWhenAHeaderIsLonger) {
  const std::string name(700, 'n');
  const std::string block = rar5_file(name, 42);

  const Result<RarBlock> part =
      parse_rar_block(std::string_view(block).substr(0, 512), RarVersion::rar5);
  ASSERT_TRUE(part.has_value()) << part.error().message;
  ASSERT_GT(part->needed, 512U);
  const Result<RarBlock> whole =
      parse_rar_block(std::string_view(block).substr(0, part->needed), RarVersion::rar5);
  ASSERT_TRUE(whole.has_value()) << whole.error().message;
  EXPECT_EQ(whole->needed, 0U);
  EXPECT_EQ(whole->member.value_or(ArchiveMember{}).name, name);
  EXPECT_EQ(whole->size, block.size());

  const std::string old = rar4_file(name, 42);
  const Result<RarBlock> old_part =
      parse_rar_block(std::string_view(old).substr(0, 100), RarVersion::rar4);
  ASSERT_TRUE(old_part.has_value());
  EXPECT_EQ(old_part->needed, 7U + 25U + name.size());
}

TEST(RarParsing, RefusesEncryptedHeadersAndGarbage) {
  const Result<std::vector<ArchiveMember>> rar4 =
      rar_listing(rar4_archive({rar4_file("a.bin", 5)}, 0x0080));
  ASSERT_FALSE(rar4.has_value());
  EXPECT_EQ(rar4.error().code, Errc::refused);
  const Result<std::vector<ArchiveMember>> rar5 =
      rar_listing(std::string(rar5_signature) +
                  rar5_header(4, vint(0) + vint(0) + vint(15) + std::string(16, 's')));
  ASSERT_FALSE(rar5.has_value());
  EXPECT_EQ(rar5.error().code, Errc::refused);

  EXPECT_FALSE(rar_version("PK\x03\x04").has_value());
  EXPECT_FALSE(rar_version("Rar!").has_value());
  EXPECT_FALSE(parse_rar_block(le(0, 2) + le(0x74, 1) + le(0x8000, 2) + le(3, 2), RarVersion::rar4)
                   .has_value());
  EXPECT_FALSE(
      parse_rar_block(le(0, 2) + le(0x74, 1) + le(0x8000, 2) + le(12, 2) + std::string(5, '\0'),
                      RarVersion::rar4)
          .has_value());
  EXPECT_FALSE(parse_rar_block(le(0, 4) + std::string(11, '\xff'), RarVersion::rar5).has_value());
  // A file header whose name runs past the header.
  EXPECT_FALSE(parse_rar_block(le(0, 4) + vint(6) + vint(2) + vint(0) + vint(0) + vint(5) +
                                   vint(0x20) + vint(40),
                               RarVersion::rar5)
                   .has_value());
  EXPECT_EQ(parse_rar_block(le(0, 3), RarVersion::rar5)->needed, 14U);
}

TEST(ArchiveSets, GroupsVolumesOfOneSet) {
  FakeDirectoryReader reader;
  const fs::path root = LR"(D:\)";
  reader.listings[root] = {
      testing::folder("a"),
  };
  std::vector<space::ListedEntry>& files = reader.listings[root / "a"];
  std::uint8_t id = 1;
  for (const char* name :
       {"Pack.zip", "Pack.z01", "Orphan.z01", "Kit.part2.rar", "Kit.part10.rar", "Kit.part1.rar",
        "Old.r00", "Old.rar", "Old.r01", "Other.7z", "notes.txt"}) {
    files.push_back(testing::file(name, 100_B, older, id++));
  }
  const space::ScanResult tree = scan_ok(reader, root);

  const std::vector<ArchiveSet> sets = archive_sets(tree.folders[1]);

  ASSERT_EQ(sets.size(), 3U);
  const auto names = [](const ArchiveSet& set) {
    std::vector<std::wstring> result;
    result.reserve(set.volumes.size());
    for (const ArchiveVolume& volume : set.volumes) {
      result.push_back(volume.path.filename().native());
    }
    return result;
  };
  EXPECT_EQ(sets[0].format, ArchiveFormat::rar);
  EXPECT_EQ(names(sets[0]),
            (std::vector<std::wstring>{L"Kit.part1.rar", L"Kit.part2.rar", L"Kit.part10.rar"}));
  EXPECT_EQ(names(sets[1]), (std::vector<std::wstring>{L"Old.rar", L"Old.r00", L"Old.r01"}));
  EXPECT_EQ(sets[2].format, ArchiveFormat::zip);
  EXPECT_EQ(names(sets[2]), (std::vector<std::wstring>{L"Pack.zip", L"Pack.z01"}));
  EXPECT_EQ(sets[2].size, 200_B);
}

// --- The analyzer ---

// Archive bytes in memory.
class FakeReader final : public space::IContentReader {
 public:
  std::map<fs::path, std::string> files;
  mutable std::uint64_t asked = 0;

  [[nodiscard]] Result<std::string> read(const fs::path& file, std::uint64_t offset,
                                         std::size_t length) const override {
    asked += length;
    const auto found = files.find(file);
    if (found == files.end()) {
      return fail(Errc::refused, "cloud file, not read");
    }
    if (offset >= found->second.size()) {
      return std::string();
    }
    return found->second.substr(static_cast<std::size_t>(offset), length);
  }
};

// One drive's folder tree.
struct Volume {
  fs::path root;
  std::uint64_t serial = 0;
  FakeDirectoryReader reader;
  std::uint8_t next_id = 1;

  void add(const fs::path& folder, const std::string& name, std::uint64_t size) {
    for (fs::path path = folder; path != root && path.has_relative_path();
         path = path.parent_path()) {
      std::vector<space::ListedEntry>& parent = reader.listings[path.parent_path()];
      const std::string leaf = path.filename().string();
      if (std::ranges::find(parent, fs::path(leaf), &space::ListedEntry::name) == parent.end()) {
        parent.push_back(testing::folder(leaf));
      }
    }
    reader.listings[folder].push_back(testing::file(name, Bytes{size}, older, next_id++, serial));
  }

  void add_archive(FakeReader& archives, const fs::path& folder, const std::string& name,
                   const std::string& bytes) {
    add(folder, name, bytes.size());
    archives.files[folder / name] = bytes;
  }

  [[nodiscard]] space::ScanResult tree() {
    reader.listings.try_emplace(root);
    return scan_ok(reader, root);
  }
};

const ArchiveOptions tiny_archives{.min_size = 1_B, .min_member = 100_B};

TEST(ExtractedArchives, ZipUnpackedBesideItIsReviewed) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add_archive(archives, LR"(D:\Assets)", "Pack.zip",
                zip_file({{"Pack/a.bin", 3000}, {"Pack/sub/b.bin", 2000}, {"readme.txt", 10}}));
  d.add(LR"(D:\Assets\Pack)", "a.bin", 3000);
  d.add(LR"(D:\Assets\Pack\sub)", "b.bin", 2000);
  const std::vector<space::ScanResult> trees = {d.tree()};

  const ArchiveRun run = find_extracted_archives(trees, archives, tiny_archives);

  EXPECT_EQ(run.listed, 1U);
  EXPECT_EQ(run.unreadable, 0U);
  ASSERT_EQ(run.findings.size(), 1U);
  const Finding& finding = run.findings[0];
  EXPECT_EQ(finding.group, ActionGroup::manual_review);
  EXPECT_EQ(finding.source, "archives");
  EXPECT_EQ(finding.drive, fs::path(LR"(D:\)"));
  EXPECT_EQ(finding.path, fs::path(LR"(D:\Assets\Pack.zip)"));
  EXPECT_EQ(finding.plan.kind, PlanKind::review);
  EXPECT_EQ(*finding.plan.param("extracted"), R"(D:\Assets\Pack)");
  ASSERT_EQ(finding.items.size(), 1U);
  EXPECT_NE(finding.reason.find("100 % of the 4.9 KiB in its 2 members"), std::string::npos)
      << finding.reason;
  EXPECT_EQ(finding.reason.find("backup"), std::string::npos);
  EXPECT_GT(run.read, 0_B);
}

TEST(ExtractedArchives, LessThanTheShareUnpackedIsNotReviewed) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add_archive(archives, LR"(D:\Assets)", "Pack.zip",
                zip_file({{"a.bin", 3000}, {"b.bin", 2000}}));
  d.add(LR"(D:\Assets\Pack)", "a.bin", 3000);
  d.add(LR"(D:\Assets\Pack)", "b.bin", 1999);  // another size: not the member
  const std::vector<space::ScanResult> trees = {d.tree()};

  const ArchiveRun run = find_extracted_archives(trees, archives, tiny_archives);

  EXPECT_EQ(run.listed, 1U);
  EXPECT_TRUE(run.findings.empty());
}

TEST(ExtractedArchives, MultiPartRarAcrossDrivesAndOnlyInABackup) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  Volume e{.root = LR"(E:\)", .serial = 5};
  // The split file has a header in both volumes; Kit/c.bin is in the second only.
  d.add_archive(archives, LR"(D:\Dl)", "Kit.part1.rar",
                rar5_archive({rar5_file("Kit/a.bin", 4000), rar5_file("Kit/split.bin", 6000)}));
  d.add_archive(archives, LR"(D:\Dl)", "Kit.part2.rar",
                rar5_archive({rar5_file("Kit/split.bin", 6000), rar5_file("Kit/c.bin", 1000)}));
  e.add(LR"(E:\_Backups\2024\Kit)", "a.bin", 4000);
  e.add(LR"(E:\_Backups\2024\Kit)", "split.bin", 6000);
  e.add(LR"(E:\_Backups\2024\Kit)", "c.bin", 1000);
  const std::vector<space::ScanResult> trees = {d.tree(), e.tree()};

  const ArchiveRun run = find_extracted_archives(trees, archives, tiny_archives);

  ASSERT_EQ(run.findings.size(), 1U);
  const Finding& finding = run.findings[0];
  EXPECT_EQ(finding.path, fs::path(LR"(D:\Dl\Kit.part1.rar)"));
  ASSERT_EQ(finding.items.size(), 2U);
  EXPECT_EQ(finding.bytes, Bytes{archives.files.at(LR"(D:\Dl\Kit.part1.rar)").size() +
                                 archives.files.at(LR"(D:\Dl\Kit.part2.rar)").size()});
  EXPECT_EQ(*finding.plan.param("extracted"), R"(E:\_Backups\2024\Kit)");
  EXPECT_NE(finding.reason.find("in its 3 members"), std::string::npos) << finding.reason;
  EXPECT_NE(finding.reason.find("only inside a backup"), std::string::npos) << finding.reason;
}

TEST(ExtractedArchives, PrefersACopyOutsideABackupAndIgnoresTheArchiveItself) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add_archive(archives, LR"(D:\A)", "Set.zip", zip_file({{"x.bin", 900}}));
  d.add(LR"(D:\Backup\Set)", "x.bin", 900);
  d.add(LR"(D:\Live\Set)", "x.bin", 900);
  // A member with the archive's own name and size never matches the archive itself.
  const std::size_t self_size = zip_file({{"Self.zip", 0}}).size();
  d.add_archive(archives, LR"(D:\B)", "Self.zip", zip_file({{"Self.zip", self_size}}));
  const std::vector<space::ScanResult> trees = {d.tree()};

  const ArchiveRun run = find_extracted_archives(trees, archives, tiny_archives);

  EXPECT_EQ(run.listed, 2U);
  ASSERT_EQ(run.findings.size(), 1U);
  EXPECT_EQ(*run.findings[0].plan.param("extracted"), R"(D:\Live\Set)");
  EXPECT_EQ(run.findings[0].reason.find("backup"), std::string::npos);
}

TEST(ExtractedArchives, EncryptedGarbageAndCloudArchivesAreUnreadable) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add_archive(archives, LR"(D:\A)", "Locked.rar",
                rar4_archive({rar4_file("a.bin", 3000)}, 0x0080));
  d.add_archive(archives, LR"(D:\A)", "Broken.zip", std::string(4000, 'g'));
  d.add(LR"(D:\A)", "Cloud.zip", 5000);  // the reader refuses it
  d.add(LR"(D:\A)", "a.bin", 3000);
  const std::vector<space::ScanResult> trees = {d.tree()};

  const ArchiveRun run = find_extracted_archives(trees, archives, tiny_archives);

  EXPECT_EQ(run.listed, 0U);
  EXPECT_EQ(run.unreadable, 3U);
  EXPECT_TRUE(run.findings.empty());
}

TEST(ExtractedArchives, BudgetsAndSizeLimitBoundWhatIsRead) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add_archive(archives, LR"(D:\A)", "Big.zip",
                zip_file({{"a.bin", 3000}}) + std::string(900, 'z'));
  d.add_archive(archives, LR"(D:\A)", "Small.zip", zip_file({{"b.bin", 3000}}));
  d.add_archive(archives, LR"(D:\A)", "Tiny.zip", "x");
  const std::vector<space::ScanResult> trees = {d.tree()};

  ArchiveOptions options = tiny_archives;
  options.min_size = 100_B;  // Tiny.zip is smaller
  options.per_archive = 1000_B;
  const ArchiveRun run = find_extracted_archives(trees, archives, options);

  EXPECT_EQ(run.skipped, 1U);  // Big.zip's tail does not fit
  EXPECT_EQ(run.listed, 1U);
  EXPECT_EQ(run.unreadable, 0U);
  EXPECT_LE(archives.asked, 1000U);
}

TEST(ExtractedArchives, StopsWhenAsked) {
  FakeReader archives;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add_archive(archives, LR"(D:\A)", "Pack.zip", zip_file({{"a.bin", 3000}}));
  d.add(LR"(D:\A\Pack)", "a.bin", 3000);
  const std::vector<space::ScanResult> trees = {d.tree()};
  std::stop_source stop;
  stop.request_stop();

  const ArchiveRun run = find_extracted_archives(trees, archives, tiny_archives, stop.get_token());

  EXPECT_TRUE(run.cancelled);
  EXPECT_EQ(run.listed, 0U);
  EXPECT_EQ(archives.asked, 0U);
  EXPECT_TRUE(run.findings.empty());
}

}  // namespace
}  // namespace myclean::scanall