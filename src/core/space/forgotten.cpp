#include "core/space/forgotten.hpp"

#include <algorithm>
#include <chrono>
#include <span>
#include <vector>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

namespace myclean::space {

std::vector<ForgottenFile> forgotten_files(std::span<const ScanResult> scans, WallTime now,
                                           Bytes min_size, std::chrono::days idle) {
  std::vector<ForgottenFile> forgotten;
  for (const ScanResult& scan : scans) {
    for (const Folder& folder : scan.folders) {
      for (const FileItem& file : folder.files) {
        if (file.link || file.extra_link || file.placeholder || file.size_on_disk < min_size) {
          continue;
        }
        const auto age = now - std::max(file.created, file.modified);
        if (age <= idle) {
          continue;
        }
        forgotten.push_back(
            ForgottenFile{.path = path_of(folder, file),
                          .size = file.size,
                          .size_on_disk = file.size_on_disk,
                          .created = file.created,
                          .modified = file.modified,
                          .idle_days = std::chrono::floor<std::chrono::days>(age).count()});
      }
    }
  }
  std::ranges::stable_sort(forgotten, std::ranges::greater{}, &ForgottenFile::size_on_disk);
  return forgotten;
}

}  // namespace myclean::space
