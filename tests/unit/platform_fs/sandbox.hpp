#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

namespace myclean::platform {

// A fresh folder %TEMP%\myclean-it\<unique> (or <parent>\myclean-it\<unique>), removed with
// everything in it on destruction. Tests create links only to targets inside it, so nothing
// outside it is ever touched.
class Sandbox {
 public:
  Sandbox();
  explicit Sandbox(const std::filesystem::path& parent);
  ~Sandbox();
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;
  Sandbox(Sandbox&&) = delete;
  Sandbox& operator=(Sandbox&&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

// The folder named by MYCLEAN_TEST_SECOND_VOLUME when it exists on another volume than %TEMP%:
// where a test may make a second sandbox. nullopt otherwise (the test is skipped).
[[nodiscard]] std::optional<std::filesystem::path> second_volume();

// Writes a file of `size` bytes, creating its parent folders.
void write_file(const std::filesystem::path& path, std::size_t size);

// Makes `link` (a new folder) a junction to the folder `target`, as `mklink /J` does.
// Returns false and leaves `link` an empty folder when the file system refuses.
[[nodiscard]] bool make_junction(const std::filesystem::path& link,
                                 const std::filesystem::path& target);

// True when the entry exists itself; a link is not followed.
[[nodiscard]] bool entry_exists(const std::filesystem::path& path);

}  // namespace myclean::platform
