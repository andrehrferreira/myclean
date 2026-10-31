#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/index/mft.hpp"
#include "core/model/result.hpp"
#include "core/space/scanner.hpp"
#include "ntfs_image.hpp"

namespace myclean::index::testing {

inline constexpr std::uint32_t recall_on_data_access = 0x400000;
inline constexpr std::uint32_t reparse_attribute = 0x400;

// One file or folder of a test volume.
struct Entry {
  std::uint64_t record = 0;
  std::uint64_t parent = root_record;  // record of the folder
  std::u16string name;
  bool directory = false;
  std::uint64_t size = 0;
  std::uint64_t allocated = 0;  // 0 with a size: resident
  std::uint32_t attributes = archive;
  std::uint32_t reparse_tag = 0;
  bool wof = false;  // Windows-compressed: `allocated` is the WofCompressedData stream
  std::uint16_t sequence = 1;
  std::vector<std::pair<std::uint64_t, std::u16string>> links;  // further hard links
};

[[nodiscard]] inline std::uint64_t parent_ref(std::uint64_t record) {
  return record == root_record ? root_ref : ref(record);
}

[[nodiscard]] inline RecordBuilder record_of_entry(const Entry& entry) {
  std::uint32_t attributes = entry.attributes;
  if (entry.directory) {
    attributes |= 0x10;
  }
  if (entry.reparse_tag != 0) {
    attributes |= reparse_attribute;
  }
  RecordBuilder builder{entry.record, entry.sequence};
  builder.standard_information(attributes);
  if (entry.directory) {
    builder.directory();
  }
  builder.file_name(parent_ref(entry.parent), entry.name);
  for (const auto& [parent, name] : entry.links) {
    builder.file_name(parent_ref(parent), name, posix);
  }
  if (!entry.directory) {
    if (entry.wof) {
      builder.data(u"", entry.size, 0, {Extent{.clusters = (entry.size + 4095) / 4096}}, sparse, 0);
      builder.data(u"WofCompressedData", entry.allocated, entry.allocated);
    } else if (entry.allocated == 0) {
      builder.resident_data(u"", entry.size);
    } else {
      builder.data(u"", entry.size, entry.allocated);
    }
  }
  if (entry.reparse_tag != 0) {
    builder.reparse_point(entry.reparse_tag);
  }
  return builder;
}

// A volume tree that can be written into a FakeVolume and listed by a directory reader, so that
// the index and scanner v1 can be compared on the same content.
class TreeModel final : public space::IDirectoryReader {
 public:
  explicit TreeModel(std::vector<Entry> entries) : entries_(std::move(entries)) {}

  // Writes the root folder and every entry into `volume`.
  void install(FakeVolume& volume) const {
    volume.put(
        root_record,
        RecordBuilder{root_record, root_record}.directory().standard_information(0x16).file_name(
            root_ref, u"."));
    for (const Entry& entry : entries_) {
      volume.put(entry.record, record_of_entry(entry));
    }
  }

  [[nodiscard]] Result<std::vector<space::ListedEntry>> list(
      const std::filesystem::path& path) const override {
    ++lists_;
    std::vector<space::ListedEntry> listing;
    const std::filesystem::path root{"C:\\"};
    if (path == root) {
      // $MFT, written by the FakeVolume itself (64 records of 1 KiB).
      listing.push_back(space::ListedEntry{.name = "$MFT",
                                           .size = Bytes{65536},
                                           .size_on_disk = Bytes{65536},
                                           .created = time(),
                                           .modified = time(),
                                           .identity = identity(ref(0))});
    }
    for (const Entry& entry : entries_) {
      add_names(entry, path, listing);
    }
    return listing;
  }

  // How many folders were listed so far.
  [[nodiscard]] std::size_t listed_count() const { return lists_; }

 private:
  static WallTime time() { return WallTime{std::chrono::seconds{unix_time}}; }

  static FileIdentity identity(std::uint64_t reference) {
    FileIdentity id{.volume_serial = serial};
    for (std::size_t i = 0; i < 8; ++i) {
      id.file_id.at(i) = static_cast<std::uint8_t>(reference >> (8U * i));
    }
    return id;
  }

  // The folder path of record `record`, empty when it is not a folder of the tree.
  // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the test tree
  [[nodiscard]] std::filesystem::path folder_path(std::uint64_t record) const {
    if (record == root_record) {
      return "C:\\";
    }
    for (const Entry& entry : entries_) {
      if (entry.record == record && entry.directory && entry.reparse_tag == 0) {
        const std::filesystem::path parent = folder_path(entry.parent);
        return parent.empty() ? parent : parent / entry.name;
      }
    }
    return {};
  }

  void add_names(const Entry& entry, const std::filesystem::path& folder,
                 std::vector<space::ListedEntry>& listing) const {
    std::vector<std::pair<std::uint64_t, std::u16string>> names{{entry.parent, entry.name}};
    names.insert(names.end(), entry.links.begin(), entry.links.end());
    const bool placeholder = (entry.attributes & recall_on_data_access) != 0;
    for (const auto& [parent, name] : names) {
      if (folder_path(parent) != folder) {
        continue;
      }
      listing.push_back(
          space::ListedEntry{.name = std::filesystem::path{name},
                             .directory = entry.directory,
                             .attributes = {.reparse_point = entry.reparse_tag != 0,
                                            .recall_on_data_access = placeholder},
                             .reparse_tag = entry.reparse_tag,
                             .size = Bytes{entry.directory ? 0 : entry.size},
                             .size_on_disk = Bytes{entry.directory ? 0 : entry.allocated},
                             .created = time(),
                             .modified = time(),
                             .identity = identity(ref(entry.record, entry.sequence))});
    }
  }

  std::vector<Entry> entries_;
  mutable std::atomic<std::size_t> lists_{0};
};

// Fails the test where two scan results of the same tree differ (folders matched by path).
inline void expect_same_tree(const space::ScanResult& actual, const space::ScanResult& expected) {
  EXPECT_EQ(actual.link_count, expected.link_count);
  EXPECT_EQ(actual.placeholder_count, expected.placeholder_count);
  EXPECT_EQ(actual.error_count, expected.error_count);
  ASSERT_EQ(actual.folders.size(), expected.folders.size());
  std::map<std::filesystem::path, const space::Folder*> by_path;
  for (const space::Folder& folder : expected.folders) {
    by_path[folder.path] = &folder;
  }
  const auto path_of = [](const space::ScanResult& scan, std::size_t index) {
    return index == space::no_parent ? std::filesystem::path{} : scan.folders[index].path;
  };
  for (const space::Folder& folder : actual.folders) {
    SCOPED_TRACE(folder.path.string());
    const auto found = by_path.find(folder.path);
    ASSERT_NE(found, by_path.end());
    const space::Folder& other = *found->second;
    EXPECT_EQ(path_of(actual, folder.parent), path_of(expected, other.parent));
    ASSERT_EQ(folder.folders.size(), other.folders.size());
    for (std::size_t i = 0; i < folder.folders.size(); ++i) {
      EXPECT_EQ(path_of(actual, folder.folders[i]), path_of(expected, other.folders[i]));
    }
    EXPECT_EQ(folder.size_on_disk, other.size_on_disk);
    EXPECT_EQ(folder.size, other.size);
    EXPECT_EQ(folder.own_size_on_disk, other.own_size_on_disk);
    EXPECT_EQ(folder.file_count, other.file_count);
    EXPECT_EQ(folder.folder_count, other.folder_count);
    ASSERT_EQ(folder.files.size(), other.files.size());
    for (std::size_t i = 0; i < folder.files.size(); ++i) {
      const space::FileItem& file = folder.files[i];
      const space::FileItem& expected_file = other.files[i];
      SCOPED_TRACE(file.name.string());
      EXPECT_EQ(file.name, expected_file.name);
      EXPECT_EQ(file.link, expected_file.link);
      EXPECT_EQ(file.placeholder, expected_file.placeholder);
      EXPECT_EQ(file.extra_link, expected_file.extra_link);
      EXPECT_EQ(file.reparse_tag, expected_file.reparse_tag);
      EXPECT_EQ(file.size, expected_file.size);
      EXPECT_EQ(file.size_on_disk, expected_file.size_on_disk);
      EXPECT_EQ(file.created, expected_file.created);
      EXPECT_EQ(file.modified, expected_file.modified);
      EXPECT_EQ(file.identity, expected_file.identity);
    }
  }
}

}  // namespace myclean::index::testing
