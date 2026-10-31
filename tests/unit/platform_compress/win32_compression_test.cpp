#include "platform/compress/win32_compression.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/compress/analyzer.hpp"
#include "core/compress/compress_folder.hpp"
#include "core/compress/compression.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/space/scanner.hpp"
#include "platform/scan/directory_reader.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using compress::Algorithm;
using testing::Sandbox;

constexpr std::uint64_t kib = 1024;

Result<compress::FileState> state_of(const fs::path& path) {
  return Win32Wof{}.state(path);
}

// What the probe predicts WOF writes for a file of `size` bytes it sampled whole.
double estimated(const compress::SampleSizes& sizes, Algorithm algorithm, double size) {
  switch (algorithm) {
    case Algorithm::xpress4k:
      return static_cast<double>(sizes.xpress4k.count());
    case Algorithm::xpress8k:
      return static_cast<double>(sizes.xpress8k.count());
    case Algorithm::xpress16k:
      return static_cast<double>(sizes.xpress16k.count());
    case Algorithm::lzx:
    case Algorithm::lznt1:
      break;
  }
  return size - (compress::lzx_gain * (size - static_cast<double>(sizes.xpress32k.count())));
}

TEST(Win32CompressionProbe, MeasuresTextAsCompressibleAndRandomBytesAsNot) {
  const Sandbox sandbox;
  const fs::path text = sandbox.root() / "text.txt";
  const fs::path noise = sandbox.root() / "noise.bin";
  testing::write_file(text, testing::text_content(256 * kib, 1));
  testing::write_file(noise, testing::random_content(256 * kib, 2));
  const Win32CompressionProbe probe;

  const Result<compress::SampleSizes> words = probe.sample(text, 0, 64 * kib);
  ASSERT_TRUE(words) << words.error().message;
  EXPECT_EQ(words->read, Bytes{64 * kib});
  EXPECT_LT(words->xpress4k.count(), 40 * kib);
  EXPECT_LE(words->xpress8k, words->xpress4k);
  EXPECT_LE(words->xpress16k, words->xpress8k);
  EXPECT_LE(words->xpress32k, words->xpress16k);
  EXPECT_LT(words->lznt1.count(), 64 * kib);

  const Result<compress::SampleSizes> random = probe.sample(noise, 0, 64 * kib);
  ASSERT_TRUE(random) << random.error().message;
  EXPECT_EQ(random->xpress4k, Bytes{64 * kib});
  EXPECT_EQ(random->lznt1, Bytes{64 * kib});

  const Result<compress::SampleSizes> tail = probe.sample(text, (192 * kib) + 100, 64 * kib);
  ASSERT_TRUE(tail);
  EXPECT_EQ(tail->read, Bytes{(64 * kib) - 100});
}

TEST(Win32CompressionProbe, RefusesACloudFile) {
  const Sandbox sandbox;
  const fs::path file = sandbox.root() / "cloud.txt";
  testing::write_file(file, testing::text_content(64 * kib, 3));
  ASSERT_TRUE(testing::mark_offline(file));

  const Result<compress::SampleSizes> sizes = Win32CompressionProbe{}.sample(file, 0, 64 * kib);
  ASSERT_FALSE(sizes);
  EXPECT_EQ(sizes.error().code, Errc::refused);
}

// The probe's XPRESS sizes are what WOF writes: within 3 % of the file, per algorithm.
TEST(Win32Wof, CompressesWithEachAlgorithmAsEstimatedAndRestoresTheContentExactly) {
  const Sandbox sandbox;
  const fs::path file = sandbox.root() / "tool.dll";
  const std::string content = testing::text_content(1024 * kib, 4);
  testing::write_file(file, content);
  const std::int64_t written = testing::last_write(file);
  const Result<compress::SampleSizes> estimate =
      Win32CompressionProbe{}.sample(file, 0, content.size());
  ASSERT_TRUE(estimate);
  Win32Wof wof;

  for (const Algorithm algorithm :
       {Algorithm::xpress4k, Algorithm::xpress8k, Algorithm::xpress16k, Algorithm::lzx}) {
    SCOPED_TRACE(std::string(compress::to_string(algorithm)));
    const Result<compress::FileState> before = state_of(file);
    ASSERT_TRUE(before) << before.error().message;
    EXPECT_FALSE(before->external);

    const Result<bool> compressed = wof.compress(file, algorithm, before->identity);
    ASSERT_TRUE(compressed) << compressed.error().message;
    EXPECT_TRUE(*compressed);
    const Result<compress::FileState> after = state_of(file);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->wof, algorithm);
    EXPECT_EQ(after->size, before->size);
    EXPECT_LT(after->size_on_disk, before->size_on_disk);
    EXPECT_EQ(testing::read_file(file), content);
    EXPECT_EQ(testing::last_write(file), written);
    const double gap =
        std::abs(static_cast<double>(after->size_on_disk.count()) -
                 estimated(*estimate, algorithm, static_cast<double>(content.size()))) /
        static_cast<double>(content.size());
    RecordProperty(std::string(compress::to_string(algorithm)) + "_gap", std::to_string(gap));
    if (algorithm != Algorithm::lzx) {
      EXPECT_LT(gap, 0.03);
    }

    ASSERT_TRUE(wof.decompress(file));
    const Result<compress::FileState> restored = state_of(file);
    ASSERT_TRUE(restored);
    EXPECT_FALSE(restored->external);
    EXPECT_GE(restored->size_on_disk, restored->size);
    EXPECT_EQ(testing::read_file(file), content);
    EXPECT_EQ(testing::last_write(file), written);
  }
  EXPECT_TRUE(wof.decompress(file));  // nothing to decompress: no error
}

TEST(Win32Wof, LeavesAFileThatDoesNotCompressAsItWas) {
  const Sandbox sandbox;
  const fs::path file = sandbox.root() / "noise.bin";
  const std::string content = testing::random_content(256 * kib, 5);
  testing::write_file(file, content);
  const Result<compress::FileState> before = state_of(file);
  ASSERT_TRUE(before);

  const Result<bool> compressed = Win32Wof{}.compress(file, Algorithm::lzx, before->identity);

  ASSERT_TRUE(compressed) << compressed.error().message;
  EXPECT_FALSE(*compressed);
  EXPECT_FALSE(state_of(file)->external);
  EXPECT_EQ(testing::read_file(file), content);
}

TEST(Win32Wof, RefusesAFileInUseAnotherFileAndLznt1) {
  const Sandbox sandbox;
  const fs::path file = sandbox.root() / "open.dll";
  testing::write_file(file, testing::text_content(128 * kib, 6));
  const Result<compress::FileState> before = state_of(file);
  ASSERT_TRUE(before);
  Win32Wof wof;

  {
    const wil::unique_hfile writer{::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                 OPEN_EXISTING, 0, nullptr)};
    ASSERT_TRUE(writer);
    const Result<bool> in_use = wof.compress(file, Algorithm::xpress4k, before->identity);
    ASSERT_FALSE(in_use);
    EXPECT_EQ(in_use.error().code, Errc::in_use);
  }
  compress::FileState other = *before;
  other.identity.file_id.at(15) ^= 0xFFU;
  const Result<bool> replaced = wof.compress(file, Algorithm::xpress4k, other.identity);
  ASSERT_FALSE(replaced);
  EXPECT_EQ(replaced.error().code, Errc::refused);
  const Result<bool> lznt1 = wof.compress(file, Algorithm::lznt1, before->identity);
  ASSERT_FALSE(lznt1);
  EXPECT_EQ(lznt1.error().code, Errc::invalid_argument);
  EXPECT_FALSE(state_of(file)->external);
}

TEST(Win32Wof, ReadsTheCompactOsStateWithoutChangingIt) {
  const Result<bool> on = Win32Wof{}.compact_os();
  ASSERT_TRUE(on) << on.error().message;
  RecordProperty("compact_os", *on ? "on" : "off");
  EXPECT_EQ(compact_path().filename(), fs::path{"compact.exe"});
  EXPECT_TRUE(fs::exists(compact_path()));
  const Result<Bytes> cluster = cluster_size(windows_folder());
  ASSERT_TRUE(cluster);
  EXPECT_GE(cluster->count(), 512U);
}

// The analyzer and CompressFolder over a real folder: what it estimates is what WOF saves,
// excluded files are untouched, and undo restores every byte.
TEST(CompressFolder, CompressesTheSandboxAsEstimatedAndUndoesExactly) {
  const Sandbox sandbox;
  const fs::path& root = sandbox.root();
  std::map<fs::path, std::string> eligible;
  for (std::uint32_t index = 0; index < 6; ++index) {
    const fs::path file =
        root / (index < 3 ? "bin" : "lib") / ("m" + std::to_string(index) + ".dll");
    eligible[file] = testing::text_content((200 + (80 * index)) * kib, 10 + index);
    testing::write_file(file, eligible[file]);
  }
  std::map<fs::path, std::string> untouched;
  // Allowed by the rules, but measured incompressible by the analyzer.
  untouched[root / "noise.bin"] = testing::random_content(300 * kib, 20);
  testing::write_file(root / "noise.bin", untouched[root / "noise.bin"]);
  for (const char* name : {"photo.jpg", "disk.vhdx", "app.db", "fresh.dll", "ntfs.dll"}) {
    untouched[root / name] = testing::text_content(300 * kib, 30);
    testing::write_file(root / name, untouched[root / name]);
  }
  testing::write_file(root / "fresh.dll", untouched[root / "fresh.dll"], std::chrono::hours{1});
  untouched[root / "small.txt"] = "tiny";
  testing::write_file(root / "small.txt", "tiny");
  ASSERT_TRUE(testing::compress_ntfs(root / "ntfs.dll"));
  std::map<fs::path, std::int64_t> dates;
  for (const auto& [path, content] : untouched) {
    dates[path] = testing::last_write(path);
  }

  const Win32DirectoryReader reader;
  Win32Wof wof;
  const os::SystemClock clock;
  const Result<Bytes> cluster = cluster_size(root);
  ASSERT_TRUE(cluster);
  compress::AnalyzerOptions analyzer;
  analyzer.rules = {.cluster = *cluster, .windows = windows_folder()};
  const Result<space::ScanResult> scan = space::scan(reader, root);
  ASSERT_TRUE(scan);
  const compress::FolderEstimate estimate =
      compress::estimate(*scan, 0, Win32CompressionProbe{}, clock, analyzer);
  EXPECT_EQ(estimate.eligible_files, 6U);
  EXPECT_EQ(estimate.incompressible, std::vector<std::string>{".bin"});
  EXPECT_TRUE(estimate.errors.empty());

  compress::Rules rules = analyzer.rules;
  rules.incompressible = estimate.incompressible;
  compress::CompressFolder action{reader, wof, clock, root, Algorithm::xpress4k, {.rules = rules}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 6U);
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(action.report().compressed, 6U);
  EXPECT_TRUE(action.report().errors.empty());

  const Result<Verification> verification = action.verify(*undo);
  ASSERT_TRUE(verification) << verification.error().message;
  EXPECT_EQ(verification->effect, Effect::better);
  const double saved = verification->before - verification->after;
  const auto predicted = static_cast<double>(estimate.find(Algorithm::xpress4k)->savings.count());
  RecordProperty("estimated", std::to_string(predicted));
  RecordProperty("saved", std::to_string(saved));
  EXPECT_NEAR(predicted, saved, 0.1 * saved);

  for (const auto& [path, content] : eligible) {
    EXPECT_EQ(state_of(path)->wof, Algorithm::xpress4k) << path;
    EXPECT_EQ(testing::read_file(path), content) << path;
  }
  for (const auto& [path, content] : untouched) {
    EXPECT_FALSE(state_of(path)->external) << path;
    EXPECT_EQ(testing::read_file(path), content) << path;
    EXPECT_EQ(testing::last_write(path), dates[path]) << path;
  }

  ASSERT_TRUE(action.undo(*undo));
  for (const auto& [path, content] : eligible) {
    const Result<compress::FileState> state = state_of(path);
    EXPECT_FALSE(state->external) << path;
    EXPECT_GE(state->size_on_disk, state->size) << path;
    EXPECT_EQ(testing::read_file(path), content) << path;
  }
}

}  // namespace
}  // namespace myclean::platform
