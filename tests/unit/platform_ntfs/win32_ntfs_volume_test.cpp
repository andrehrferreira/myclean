#include "platform/ntfs/win32_ntfs_volume.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>
#include <winioctl.h>

#include "core/index/indexed_volume.hpp"
#include "core/index/mft.hpp"
#include "core/index/usn.hpp"
#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/space/scanner.hpp"
#include "platform/scan/directory_reader.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::optional<std::string> environment(const char* name) {
  std::size_t length = 0;
  std::array<char, 512> value{};
  if (::getenv_s(&length, value.data(), value.size(), name) != 0 || length == 0) {
    return std::nullopt;
  }
  return std::string(value.data());
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// Below-normal CPU and background I/O for the whole process (scanner v1 starts its own threads).
void run_in_background() {
  ASSERT_NE(::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS), FALSE);
  ASSERT_NE(::SetPriorityClass(::GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN), FALSE);
}

TEST(Win32NtfsVolume, RefusesToOpenAVolumeWithoutElevation) {
  if (process_elevated()) {
    GTEST_SKIP() << "elevated: the volume opens";
  }
  const Result<std::unique_ptr<Win32NtfsVolume>> volume = Win32NtfsVolume::open("C:\\");
  ASSERT_FALSE(volume);
  EXPECT_EQ(volume.error().code, Errc::access_denied);
  EXPECT_NE(volume.error().message.find("elevated"), std::string::npos);
}

TEST(Win32NtfsVolume, RefusesAPathThatIsNotADriveRoot) {
  EXPECT_EQ(Win32NtfsVolume::open("relative").error().code, Errc::invalid_argument);
  EXPECT_EQ(Win32NtfsVolume::open("\\\\server\\share\\").error().code, Errc::invalid_argument);
}

// FSCTL_READ_FILE_USN_DATA works on a file handle without elevation: the USN record parser
// reads what Windows itself returns for a fresh file.
TEST(Win32NtfsVolume, ParsesTheUsnRecordWindowsReturnsForAFile) {
  const fs::path folder = fs::temp_directory_path() / "myclean-it-ntfs";
  fs::create_directories(folder);
  const fs::path file = folder / L"r\u00e9sum\u00e9 usn.txt";
  std::ofstream{file} << "usn";
  const auto handle_of = [](const fs::path& path) {
    return wil::unique_hfile{::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  };
  const wil::unique_hfile handle = handle_of(file);
  const wil::unique_hfile parent = handle_of(folder);
  ASSERT_TRUE(handle && parent);
  std::vector<std::byte> output(4096);
  DWORD returned = 0;
  ASSERT_NE(::DeviceIoControl(handle.get(), FSCTL_READ_FILE_USN_DATA, nullptr, 0, output.data(),
                              static_cast<DWORD>(output.size()), &returned, nullptr),
            FALSE);
  output.resize(returned);
  BY_HANDLE_FILE_INFORMATION own{};
  BY_HANDLE_FILE_INFORMATION folder_info{};
  ASSERT_NE(::GetFileInformationByHandle(handle.get(), &own), FALSE);
  ASSERT_NE(::GetFileInformationByHandle(parent.get(), &folder_info), FALSE);

  const Result<std::vector<index::UsnRecord>> records = index::parse_usn_records(output);
  ASSERT_TRUE(records) << records.error().message;
  ASSERT_EQ(records->size(), 1U);
  const index::UsnRecord& record = records->front();
  EXPECT_EQ(record.name, "r\xC3\xA9sum\xC3\xA9 usn.txt");
  EXPECT_EQ(record.file, (std::uint64_t{own.nFileIndexHigh} << 32U) | own.nFileIndexLow);
  EXPECT_EQ(record.parent,
            (std::uint64_t{folder_info.nFileIndexHigh} << 32U) | folder_info.nFileIndexLow);
  fs::remove_all(folder);
}

TEST(Win32NtfsVolume, ReadsTheRootRecordAndTheJournalOfC) {
  if (!process_elevated()) {
    GTEST_SKIP() << R"(not elevated: opening \\.\C: for the $MFT needs an elevated process)";
  }
  const std::unique_ptr<Win32NtfsVolume> volume = Win32NtfsVolume::open("C:\\").value();
  const index::VolumeGeometry geometry = volume->geometry().value();
  EXPECT_TRUE(geometry.bytes_per_record == 1024 || geometry.bytes_per_record == 4096);
  EXPECT_GT(geometry.mft_valid_length, 0U);

  std::optional<std::vector<std::byte>> root = volume->file_record(index::root_record).value();
  ASSERT_TRUE(root.has_value());
  index::RecordSegment segment;
  ASSERT_TRUE(index::parse_record(*root, segment));
  EXPECT_TRUE(segment.in_use && segment.directory);

  const std::optional<index::JournalState> journal = volume->journal().value();
  ASSERT_TRUE(journal.has_value());  // C: always has a change journal
  const index::JournalState state = journal.value_or(index::JournalState{});
  const std::vector<std::byte> tail = volume->read_journal(state.id, state.next_usn).value();
  EXPECT_TRUE(index::parse_usn_buffer(tail));
}

// X7, scanner v1 rate: MYCLEAN_X7_FOLDER=<folder> runs scanner v1 over it at background priority
// and prints the rate. Skipped unless the variable is set (bounded folders only).
TEST(X7, MeasuresScannerV1OnAFolder) {
  const std::optional<std::string> folder = environment("MYCLEAN_X7_FOLDER");
  if (!folder) {
    GTEST_SKIP() << "set MYCLEAN_X7_FOLDER to a folder to measure scanner v1";
  }
  run_in_background();
  const Win32DirectoryReader reader;
  for (const unsigned workers : {1U, 4U, 8U}) {
    const Clock::time_point start = Clock::now();
    const space::ScanResult scan =
        space::scan(reader, fs::path{*folder}, space::ScanOptions{.workers = workers}).value();
    const double seconds = seconds_since(start);
    const std::uint64_t entries = scan.root().file_count + scan.root().folder_count;
    std::cout << "X7 scanner v1 " << *folder << " workers=" << workers
              << ": folders=" << scan.root().folder_count << " files=" << scan.root().file_count
              << " errors=" << scan.error_count << " seconds=" << seconds
              << " entries/s=" << static_cast<double>(entries) / seconds << "\n";
  }
}

// X7, $MFT index: MYCLEAN_X7_VOLUME=C: (elevated) reads the whole $MFT at background priority,
// times FSCTL_ENUM_USN_DATA for comparison, refreshes from the journal and, with
// MYCLEAN_X7_FOLDER set to a folder of that volume, compares the index with scanner v1 there.
TEST(X7, MeasuresTheMftIndexOfAVolume) {
  const std::optional<std::string> drive = environment("MYCLEAN_X7_VOLUME");
  if (!drive) {
    GTEST_SKIP() << "set MYCLEAN_X7_VOLUME (for example C:) and run elevated";
  }
  if (!process_elevated()) {
    GTEST_SKIP() << "not elevated: opening the volume for the $MFT needs an elevated process";
  }
  run_in_background();
  const std::unique_ptr<Win32NtfsVolume> volume =
      Win32NtfsVolume::open(fs::path{*drive + "\\"}).value();
  const index::VolumeGeometry geometry = volume->geometry().value();

  Clock::time_point start = Clock::now();
  Result<index::IndexedVolume> indexed = index::IndexedVolume::build(*volume);
  const double build_seconds = seconds_since(start);
  ASSERT_TRUE(indexed) << indexed.error().message;
  const index::VolumeIndex& index = indexed->index();
  const std::uint64_t records = geometry.mft_valid_length / geometry.bytes_per_record;
  std::cout << "X7 $MFT " << *drive << ": " << geometry.mft_valid_length / (1U << 20U) << " MiB, "
            << records << " records, read and indexed in " << build_seconds << " s ("
            << static_cast<double>(records) / build_seconds << " records/s); " << index.node_count()
            << " names, " << index.unparented_count() << " unparented, "
            << static_cast<double>(index.memory_bytes()) / static_cast<double>(index.node_count())
            << " bytes/name, " << index.size_on_disk().count() / (1ULL << 30U) << " GiB on disk\n";

  start = Clock::now();
  std::uint64_t enumerated = 0;
  ASSERT_TRUE(index::enumerate_files(*volume, [&](const index::UsnRecord&) { ++enumerated; }));
  std::cout << "X7 FSCTL_ENUM_USN_DATA " << *drive << ": " << enumerated << " files in "
            << seconds_since(start) << " s (names only, no sizes)\n";
  const space::ScanResult whole = index.to_scan_result(fs::path{*drive + "\\"}, false).value();
  const std::uint64_t indexed_entries =
      whole.root().folder_count + 1 + whole.link_count + whole.root().file_count;
  std::cout << "X7 index entries (names): " << indexed_entries << "\n";
  EXPECT_GE(indexed_entries, enumerated - (enumerated / 100));  // hard links add names

  start = Clock::now();
  const index::RefreshOutcome refreshed = indexed->refresh(*volume).value();
  std::cout << "X7 journal refresh: " << refreshed.records << " records in " << seconds_since(start)
            << " s"
            << (refreshed.kind == index::RefreshKind::rebuilt
                    ? " (rebuilt: " + refreshed.reason + ")"
                    : std::string{})
            << "\n";

  const std::optional<std::string> folder = environment("MYCLEAN_X7_FOLDER");
  if (!folder) {
    return;
  }
  const space::ScanResult from_index = indexed->index().to_scan_result(*folder).value();
  start = Clock::now();
  const space::ScanResult walked =
      space::scan(Win32DirectoryReader{}, fs::path{*folder}, space::ScanOptions{.workers = 4})
          .value();
  std::cout << "X7 scanner v1 " << *folder << ": " << seconds_since(start) << " s\n";
  std::map<fs::path, Bytes> walked_sizes;
  for (const space::Folder& entry : walked.folders) {
    walked_sizes[entry.path] = entry.size_on_disk;
  }
  std::size_t same = 0;
  std::size_t shown = 0;
  for (const space::Folder& entry : from_index.folders) {
    const auto found = walked_sizes.find(entry.path);
    if (found != walked_sizes.end() && found->second == entry.size_on_disk) {
      ++same;
    } else if (shown++ < 10) {
      std::cout << "  differs: " << entry.path.string() << " index=" << entry.size_on_disk.count()
                << " scan="
                << (found == walked_sizes.end() ? std::string{"missing"}
                                                : std::to_string(found->second.count()))
                << "\n";
    }
  }
  std::cout << "X7 consistency " << *folder << ": " << same << " of " << from_index.folders.size()
            << " folders equal; totals index=" << from_index.root().size_on_disk.count()
            << " scan=" << walked.root().size_on_disk.count()
            << "; placeholders index=" << from_index.placeholder_count
            << " scan=" << walked.placeholder_count << "; links index=" << from_index.link_count
            << " scan=" << walked.link_count << "\n";
  EXPECT_GE(same * 100, from_index.folders.size() * 99);
}

}  // namespace
}  // namespace myclean::platform
