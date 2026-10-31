#include "core/ipc/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/model/result.hpp"

namespace myclean::ipc {

Result<std::string> encode_frame(std::string_view payload) {
  if (payload.size() > max_payload_size) {
    return fail(Errc::invalid_argument, "frame payload of " + std::to_string(payload.size()) +
                                            " bytes exceeds the limit of " +
                                            std::to_string(max_payload_size));
  }
  const auto length = static_cast<std::uint32_t>(payload.size());
  std::string frame;
  frame.reserve(frame_header_size + payload.size());
  for (std::size_t byte = 0; byte < frame_header_size; ++byte) {
    frame.push_back(static_cast<char>((length >> (8U * byte)) & 0xFFU));
  }
  frame.append(payload);
  return frame;
}

Result<std::string> decode_frame(std::string_view frame) {
  if (frame.size() < frame_header_size) {
    return fail(Errc::invalid_data, "frame of " + std::to_string(frame.size()) +
                                        " bytes is shorter than its length header");
  }
  std::uint32_t length = 0;
  for (std::size_t byte = 0; byte < frame_header_size; ++byte) {
    length |= static_cast<std::uint32_t>(static_cast<unsigned char>(frame[byte])) << (8U * byte);
  }
  const std::string_view payload = frame.substr(frame_header_size);
  if (length > max_payload_size) {
    return fail(Errc::invalid_data, "frame length " + std::to_string(length) +
                                        " exceeds the limit of " +
                                        std::to_string(max_payload_size));
  }
  if (length != payload.size()) {
    return fail(Errc::invalid_data, "frame length " + std::to_string(length) + " does not match " +
                                        std::to_string(payload.size()) + " payload bytes");
  }
  return std::string(payload);
}

}  // namespace myclean::ipc
