#include "core/index/usn.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "core/index/bytes.hpp"
#include "core/index/mft.hpp"
#include "core/model/result.hpp"
#include "ntfs_image.hpp"

namespace myclean::index {
namespace {

using testing::ref;

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
  std::vector<std::byte> bytes;
  for (const unsigned value : values) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

// FSCTL_READ_FILE_USN_DATA on C:\Windows\notepad.exe, recorded on the reference machine
// (Windows 11 26200, not elevated) during the X7 spike.
const std::vector<std::byte> recorded_notepad = bytes_of({
    0x58, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x7d, 0x46, 0x4a, 0x00, 0x00, 0x00, 0x01,
    0x00, 0xba, 0x68, 0x4a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x90, 0xff, 0xa5, 0xf0, 0x5c, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x16, 0x00, 0x3c, 0x00,
    0x6e, 0x00, 0x6f, 0x00, 0x74, 0x00, 0x65, 0x00, 0x70, 0x00, 0x61, 0x00, 0x64, 0x00, 0x2e,
    0x00, 0x65, 0x00, 0x78, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
});

TEST(ParseUsnRecords, ReadsARecordedV2Record) {
  const Result<std::vector<UsnRecord>> records = parse_usn_records(recorded_notepad);
  ASSERT_TRUE(records) << records.error().message;
  ASSERT_EQ(records->size(), 1U);
  const UsnRecord& record = records->front();
  EXPECT_EQ(record.file, 0x0001'0000'004A'467DULL);
  EXPECT_EQ(record_of(record.file), 0x4A467DU);
  EXPECT_EQ(sequence_of(record.file), 1U);
  EXPECT_EQ(record.parent, 0x0001'0000'004A'68BAULL);
  EXPECT_EQ(record.usn, 0x5C'F0A5'FF90LL);
  EXPECT_EQ(record.reason, 0U);
  EXPECT_EQ(record.attributes, 0x20U);
  EXPECT_EQ(record.name, "notepad.exe");
}

TEST(ParseUsnRecords, ReadsAV3RecordWithA64BitId) {
  const std::vector<std::byte> bytes =
      testing::usn_v3(ref(70, 4), ref(5, 5), 4096, usn_reason::file_delete | usn_reason::close,
                      testing::archive, u"gone.tmp");
  const Result<std::vector<UsnRecord>> records = parse_usn_records(bytes);
  ASSERT_TRUE(records) << records.error().message;
  ASSERT_EQ(records->size(), 1U);
  EXPECT_EQ(records->front(), (UsnRecord{.file = ref(70, 4),
                                         .parent = ref(5, 5),
                                         .usn = 4096,
                                         .reason = usn_reason::file_delete | usn_reason::close,
                                         .attributes = testing::archive,
                                         .name = "gone.tmp"}));
}

TEST(ParseUsnRecords, RejectsA128BitIdThatNtfsWouldNotHave) {
  std::vector<std::byte> bytes = testing::usn_v3(ref(70), ref(5), 1, 0, 0, u"x");
  store(std::span{bytes}, 16, std::uint64_t{1});  // high half of the file id
  EXPECT_FALSE(parse_usn_records(bytes));
}

TEST(ParseUsnRecords, SkipsARangeTrackingV4Record) {
  std::vector<std::byte> bytes(0x50);
  store(std::span{bytes}, 0, std::uint32_t{0x50});
  store(std::span{bytes}, 4, std::uint16_t{4});
  const std::vector<std::byte> v2 = testing::usn_v2(ref(80), ref(5), 8, 0, 0, u"kept");
  bytes.insert(bytes.end(), v2.begin(), v2.end());
  const Result<std::vector<UsnRecord>> records = parse_usn_records(bytes);
  ASSERT_TRUE(records);
  ASSERT_EQ(records->size(), 1U);
  EXPECT_EQ(records->front().name, "kept");
}

TEST(ParseUsnRecords, RejectsARecordLongerThanItsBuffer) {
  std::vector<std::byte> bytes = recorded_notepad;
  bytes.resize(0x40);
  EXPECT_FALSE(parse_usn_records(bytes));
}

TEST(ParseUsnBuffer, ReadsTheNextValueAndEveryRecord) {
  std::vector<std::byte> bytes(8);
  store(std::span{bytes}, 0, std::uint64_t{0x1234});
  for (const auto& record : {testing::usn_v2(ref(81), ref(5), 100, 0, 0, u"one"),
                             testing::usn_v2(ref(82), ref(5), 200, 0, 0, u"two")}) {
    bytes.insert(bytes.end(), record.begin(), record.end());
  }
  const Result<UsnBuffer> buffer = parse_usn_buffer(bytes);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(buffer->next, 0x1234U);
  ASSERT_EQ(buffer->records.size(), 2U);
  EXPECT_EQ(buffer->records[1].name, "two");
  EXPECT_EQ(buffer->records[1].usn, 200);
}

TEST(ParseUsnBuffer, RejectsABufferWithoutItsHeader) {
  EXPECT_FALSE(parse_usn_buffer(std::vector<std::byte>(4)));
}

}  // namespace
}  // namespace myclean::index
