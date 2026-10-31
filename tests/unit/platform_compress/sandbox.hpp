#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace myclean::platform::testing {

// A fresh folder %TEMP%\myclean-it\compress-<unique>, removed with everything in it on
// destruction. Every compression in these tests happens inside it.
class Sandbox {
 public:
  Sandbox();
  ~Sandbox();
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;
  Sandbox(Sandbox&&) = delete;
  Sandbox& operator=(Sandbox&&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

// `size` bytes of words drawn from a small vocabulary: compresses about as well as program files.
[[nodiscard]] std::string text_content(std::size_t size, std::uint32_t seed);
// `size` pseudo-random bytes: does not compress.
[[nodiscard]] std::string random_content(std::size_t size, std::uint32_t seed);

// Writes `content` to `path`, creating its parent folders, and dates its last write `age` ago.
void write_file(const std::filesystem::path& path, std::string_view content,
                std::chrono::hours age = std::chrono::days{60});
[[nodiscard]] std::string read_file(const std::filesystem::path& path);
// The last-write time in 100 ns ticks.
[[nodiscard]] std::int64_t last_write(const std::filesystem::path& path);

// Compresses `path` with NTFS (LZNT1) compression.
[[nodiscard]] bool compress_ntfs(const std::filesystem::path& path);
// Sets FILE_ATTRIBUTE_OFFLINE: the attribute that marks content not stored locally.
[[nodiscard]] bool mark_offline(const std::filesystem::path& path);

}  // namespace myclean::platform::testing
