#include "core/index/mft.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "ntfs_image.hpp"

namespace myclean::index {
namespace {

using testing::archive;
using testing::compressed;
using testing::RecordBuilder;
using testing::ref;
using testing::root_ref;
using testing::sparse;

RecordSegment parse(std::vector<std::byte> bytes, bool with_extents = false) {
  RecordSegment segment;
  const Result<void> parsed = parse_record(bytes, segment, with_extents);
  EXPECT_TRUE(parsed) << parsed.error().message;
  return segment;
}

TEST(ParseRecord, ReadsAFileFromTheDiskWithItsUpdateSequenceRestored) {
  // The resident value of 600 bytes crosses the first 512-byte stride, whose last two bytes
  // the disk form replaces with the update sequence number.
  const RecordBuilder builder = RecordBuilder{40, 3}
                                    .standard_information(archive)
                                    .file_name(ref(30, 2), u"report.txt")
                                    .resident_data(u"", 600);
  const RecordSegment segment = parse(builder.on_disk());

  EXPECT_TRUE(segment.in_use);
  EXPECT_FALSE(segment.directory);
  EXPECT_EQ(segment.record, 40U);
  EXPECT_EQ(segment.sequence, 3U);
  EXPECT_EQ(segment.base, 0U);
  EXPECT_EQ(segment.attributes, archive);
  EXPECT_EQ(segment.created, testing::file_time);
  ASSERT_EQ(segment.names.size(), 1U);
  EXPECT_EQ(segment.names[0].parent, ref(30, 2));
  EXPECT_EQ(segment.names[0].name, "report.txt");
  EXPECT_EQ(segment.size, 600U);
  EXPECT_EQ(segment.size_on_disk, 0U);  // resident: no clusters
}

TEST(ParseRecord, AcceptsARecordWhoseUpdateSequenceIsAlreadyRestored) {
  const RecordBuilder builder = RecordBuilder{41}.file_name(root_ref, u"a").resident_data(u"", 700);
  const RecordSegment from_disk = parse(builder.on_disk());
  const RecordSegment restored = parse(builder.restored());
  EXPECT_EQ(restored.size, from_disk.size);
  EXPECT_EQ(restored.names[0].name, from_disk.names[0].name);
}

TEST(ParseRecord, RejectsATornRecord) {
  std::vector<std::byte> bytes = RecordBuilder{42}.file_name(root_ref, u"a").on_disk();
  bytes[1022] = std::byte{0x99};  // the second stride no longer ends with the sequence number
  RecordSegment segment;
  const Result<void> parsed = parse_record(bytes, segment);
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().code, Errc::invalid_data);
}

TEST(ParseRecord, ReportsAnUnusedAndANeverUsedRecordAsNotInUse) {
  EXPECT_FALSE(parse(RecordBuilder{43}.not_in_use().file_name(root_ref, u"old").on_disk()).in_use);
  EXPECT_FALSE(parse(std::vector<std::byte>(1024)).in_use);
}

TEST(ParseRecord, RejectsABadSignature) {
  std::vector<std::byte> bytes = RecordBuilder{44}.on_disk();
  bytes[0] = std::byte{'B'};
  RecordSegment segment;
  EXPECT_FALSE(parse_record(bytes, segment));
}

TEST(ParseRecord, KeepsEveryHardLinkAndDropsTheDosName) {
  const RecordSegment segment = parse(RecordBuilder{45}
                                          .file_name(ref(30), u"Long File Name.txt", testing::win32)
                                          .file_name(ref(30), u"LONGFI~1.TXT", testing::dos)
                                          .file_name(ref(31), u"link.txt", testing::posix)
                                          .on_disk());
  ASSERT_EQ(segment.names.size(), 2U);
  EXPECT_EQ(segment.names[0].name, "Long File Name.txt");
  EXPECT_EQ(segment.names[1].name, "link.txt");
  EXPECT_EQ(segment.names[1].parent, ref(31));
}

TEST(ParseRecord, MarksADirectory) {
  EXPECT_TRUE(parse(RecordBuilder{46}.directory().file_name(root_ref, u"d").on_disk()).directory);
}

TEST(ParseRecord, CountsTheClustersOfANonResidentStream) {
  const RecordSegment segment =
      parse(RecordBuilder{47}.file_name(root_ref, u"big.bin").data(u"", 10'000, 12'288).on_disk());
  EXPECT_EQ(segment.size, 10'000U);
  EXPECT_EQ(segment.size_on_disk, 12'288U);
}

TEST(ParseRecord, CountsWhatACompressedOrSparseStreamOccupies) {
  const RecordSegment compressed_file =
      parse(RecordBuilder{48}
                .file_name(root_ref, u"c.log")
                .data(u"", 100'000, 131'072, {}, compressed, 40'960)
                .on_disk());
  EXPECT_EQ(compressed_file.size, 100'000U);
  EXPECT_EQ(compressed_file.size_on_disk, 40'960U);

  const RecordSegment sparse_file = parse(RecordBuilder{49}
                                              .file_name(root_ref, u"s.vhd")
                                              .data(u"", 1'000'000, 1'003'520, {}, sparse, 8'192)
                                              .on_disk());
  EXPECT_EQ(sparse_file.size_on_disk, 8'192U);
}

TEST(ParseRecord, CountsTheWofStreamOfAWindowsCompressedFile) {
  const RecordSegment segment =
      parse(RecordBuilder{50}
                .standard_information(archive | 0x400 | 0x200)
                .file_name(root_ref, u"app.dll")
                .data(u"", 300'000, 303'104, {Extent{.clusters = 74}}, sparse, 0)
                .reparse_point(testing::tag_wof)
                .data(u"WofCompressedData", 90'000, 90'112)
                .on_disk());
  EXPECT_EQ(segment.size, 300'000U);         // the logical size of the unnamed stream
  EXPECT_EQ(segment.size_on_disk, 90'112U);  // only the compressed stream occupies clusters
  EXPECT_EQ(segment.reparse_tag, testing::tag_wof);
}

TEST(ParseRecord, CountsOnlyTheFirstPartOfAStream) {
  const RecordSegment segment = parse(
      RecordBuilder{51}.base(ref(47)).data_part(3, {Extent{.lcn = 900, .clusters = 2}}).on_disk());
  EXPECT_EQ(segment.base, ref(47));
  EXPECT_FALSE(segment.size);
  EXPECT_EQ(segment.size_on_disk, 0U);
}

TEST(ParseRecord, TakesTheReparseTagFromTheFileNameWhenTheReparsePointIsNotResidentHere) {
  const RecordSegment segment =
      parse(RecordBuilder{52}
                .directory()
                .standard_information(0x10 | 0x400)
                .file_name(root_ref, u"junction", testing::win32, testing::tag_mount_point)
                .on_disk());
  EXPECT_EQ(segment.reparse_tag, testing::tag_mount_point);
}

TEST(ParseRecord, IgnoresTheEaSizeOfAFileThatIsNotAReparsePoint) {
  const RecordSegment segment = parse(
      RecordBuilder{53}.standard_information(archive).file_name(root_ref, u"f", 1, 64).on_disk());
  EXPECT_EQ(segment.reparse_tag, 0U);
}

TEST(ParseRecord, DecodesTheDataRunsWhenAsked) {
  const std::vector<Extent> runs{Extent{.lcn = 100, .clusters = 8},
                                 Extent{.lcn = 158, .clusters = 8}};
  const RecordSegment segment = parse(
      RecordBuilder{0}.file_name(root_ref, u"$MFT").data(u"", 65536, 65536, runs).on_disk(), true);
  ASSERT_EQ(segment.data_extents.size(), 2U);
  EXPECT_EQ(segment.data_extents[0], (Extent{.vcn = 0, .lcn = 100, .clusters = 8}));
  EXPECT_EQ(segment.data_extents[1], (Extent{.vcn = 8, .lcn = 158, .clusters = 8}));
}

TEST(ParseRecord, KeepsAResidentAttributeList) {
  std::vector<std::byte> list = testing::list_entry(0x10, ref(54));
  const std::vector<std::byte> second = testing::list_entry(0x80, ref(60));
  list.insert(list.end(), second.begin(), second.end());
  const RecordSegment segment =
      parse(RecordBuilder{54}.attribute_list(list).file_name(root_ref, u"f").on_disk());
  EXPECT_TRUE(segment.has_attribute_list);
  EXPECT_EQ(segment.attribute_list, list);
}

TEST(DecodeRuns, FollowsNegativeOffsetsAndSparseRuns) {
  // 0x21: 1 length byte, 2 offset bytes. Run 1: 16 clusters at LCN 0x1000. Run 2: 4 clusters
  // at -0x10 from there. Run 3: 8 sparse clusters (no offset).
  const std::vector<std::byte> pairs{
      std::byte{0x21}, std::byte{0x10}, std::byte{0x00}, std::byte{0x10}, std::byte{0x11},
      std::byte{0x04}, std::byte{0xF0}, std::byte{0x01}, std::byte{0x08}, std::byte{0x00}};
  const Result<std::vector<Extent>> runs = decode_runs(pairs, 5);
  ASSERT_TRUE(runs) << runs.error().message;
  ASSERT_EQ(runs->size(), 3U);
  EXPECT_EQ((*runs)[0], (Extent{.vcn = 5, .lcn = 0x1000, .clusters = 16}));
  EXPECT_EQ((*runs)[1], (Extent{.vcn = 21, .lcn = 0xFF0, .clusters = 4}));
  EXPECT_EQ((*runs)[2], (Extent{.vcn = 25, .lcn = std::nullopt, .clusters = 8}));
}

TEST(DecodeRuns, RejectsARunPastItsBuffer) {
  const std::vector<std::byte> pairs{std::byte{0x33}, std::byte{0x01}};
  EXPECT_FALSE(decode_runs(pairs, 0));
}

TEST(ExtensionRecords, ListsTheOtherSegmentsOnce) {
  std::vector<std::byte> list;
  for (const std::vector<std::byte>& entry :
       {testing::list_entry(0x10, ref(7)), testing::list_entry(0x30, ref(90)),
        testing::list_entry(0x80, ref(91)), testing::list_entry(0x80, ref(90), 40)}) {
    list.insert(list.end(), entry.begin(), entry.end());
  }
  const Result<std::vector<std::uint64_t>> records = extension_records(list, 7);
  ASSERT_TRUE(records);
  EXPECT_EQ(*records, (std::vector<std::uint64_t>{90, 91}));
}

TEST(Utf8OfUtf16, EncodesEveryPlaneAndReplacesAnUnpairedSurrogate) {
  // "a", "é" (U+00E9), "€" (U+20AC), U+1F600 as a surrogate pair, a lone high surrogate.
  const std::vector<std::byte> text{std::byte{0x61}, std::byte{0x00}, std::byte{0xE9},
                                    std::byte{0x00}, std::byte{0xAC}, std::byte{0x20},
                                    std::byte{0x3D}, std::byte{0xD8}, std::byte{0x00},
                                    std::byte{0xDE}, std::byte{0x00}, std::byte{0xD8}};
  EXPECT_EQ(utf8_of_utf16(text), "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xEF\xBF\xBD");
}

}  // namespace
}  // namespace myclean::index
