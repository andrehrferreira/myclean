#include "core/ipc/frame.hpp"

#include <string>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::ipc {
namespace {

TEST(FrameTest, PrefixesThePayloadWithItsLittleEndianLength) {
  const std::string payload(0x0102, 'x');
  const Result<std::string> frame = encode_frame(payload);
  ASSERT_TRUE(frame.has_value());
  ASSERT_EQ(frame->size(), frame_header_size + payload.size());
  EXPECT_EQ(frame->substr(0, 4), std::string("\x02\x01\x00\x00", 4));
  EXPECT_EQ(frame->substr(4), payload);
}

TEST(FrameTest, RoundTripsEmptyAndMaximalPayloads) {
  for (const std::string& payload : {std::string(), std::string(max_payload_size, '{')}) {
    const Result<std::string> frame = encode_frame(payload);
    ASSERT_TRUE(frame.has_value());
    const Result<std::string> decoded = decode_frame(*frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, payload);
  }
}

TEST(FrameTest, RefusesToEncodeAnOversizedPayload) {
  const Result<std::string> frame = encode_frame(std::string(max_payload_size + 1, 'x'));
  ASSERT_FALSE(frame.has_value());
  EXPECT_EQ(frame.error().code, Errc::invalid_argument);
}

TEST(FrameTest, RejectsMalformedFrames) {
  const std::string oversized_header("\x01\x00\x10\x00", 4);  // 1 MiB + 1
  for (const std::string& frame : {std::string("\x01\x00", 2), std::string("\x05\x00\x00\x00{}", 6),
                                   std::string("\x01\x00\x00\x00{}", 6), oversized_header}) {
    const Result<std::string> decoded = decode_frame(frame);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, Errc::invalid_data);
  }
}

}  // namespace
}  // namespace myclean::ipc
