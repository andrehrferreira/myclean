#pragma once

#include <cstddef>
#include <filesystem>

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

// Writes a file of `size` bytes, creating its parent folders.
void write_file(const std::filesystem::path& path, std::size_t size);

// Makes `link` (a new folder) a junction to the folder `target`, as `mklink /J` does.
// Returns false and leaves `link` an empty folder when the file system refuses.
[[nodiscard]] bool make_junction(const std::filesystem::path& link,
                                 const std::filesystem::path& target);

// True when the entry exists itself; a link is not followed.
[[nodiscard]] bool entry_exists(const std::filesystem::path& path);

}  // namespace myclean::platform
