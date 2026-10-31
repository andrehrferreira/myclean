#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "core/model/result.hpp"

namespace myclean::ipc {

// One message on the service pipe: the payload length as a 4-byte little-endian unsigned
// integer, followed by the payload (UTF-8 JSON-RPC 2.0 text). Each pipe message carries exactly
// one frame.
inline constexpr std::size_t frame_header_size = 4;
inline constexpr std::size_t max_payload_size = std::size_t{1} << 20U;  // 1 MiB
inline constexpr std::size_t max_frame_size = frame_header_size + max_payload_size;

// Fails with invalid_argument when `payload` is larger than max_payload_size.
[[nodiscard]] Result<std::string> encode_frame(std::string_view payload);
// The payload of `frame`. Fails with invalid_data when the frame is shorter than its header, its
// length does not match the bytes that follow, or it exceeds max_payload_size.
[[nodiscard]] Result<std::string> decode_frame(std::string_view frame);

}  // namespace myclean::ipc
