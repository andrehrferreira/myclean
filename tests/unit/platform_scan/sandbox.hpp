#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace myclean::platform {

// A fresh folder %TEMP%\myclean-it\<unique>, removed with everything in it on destruction.
// Tests create links only to targets inside it, so nothing outside it is ever touched.
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

// Writes a file of `size` bytes of `fill`, creating its parent folders (long paths allowed).
void write_file(const std::filesystem::path& path, std::size_t size, char fill = 'x');

// Writes `text` to the alternate data stream `stream` of the existing file `path`.
void write_stream(const std::filesystem::path& path, std::wstring_view stream,
                  std::string_view text);

// Makes `link` (a new folder) a junction to the folder `target`, as `mklink /J` does.
[[nodiscard]] bool make_junction(const std::filesystem::path& link,
                                 const std::filesystem::path& target);

// Makes `path` (an existing file) sparse and `size` bytes long without allocating anything.
[[nodiscard]] bool make_sparse(const std::filesystem::path& path, std::size_t size);

// Compresses the existing file `path` with NTFS (LZNT1) compression.
[[nodiscard]] bool compress_ntfs(const std::filesystem::path& path);

// Compresses the existing file `path` the way `compact /exe:xpress4k` does (WOF).
[[nodiscard]] bool compress_wof(const std::filesystem::path& path);

// Sets FILE_ATTRIBUTE_OFFLINE on `path`: the attribute that marks content not stored locally.
[[nodiscard]] bool mark_offline(const std::filesystem::path& path);

// Makes the folder `path` unlistable for everyone, or listable again.
[[nodiscard]] bool deny_listing(const std::filesystem::path& path, bool deny);

// The "\\?\" form of an absolute path, for files whose path exceeds MAX_PATH.
[[nodiscard]] std::filesystem::path extended(const std::filesystem::path& path);

}  // namespace myclean::platform
