#include "core/compress/analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/clean/probes.hpp"
#include "core/compress/compression.hpp"
#include "core/model/units.hpp"
#include "core/packs/schema.hpp"
#include "core/space/scanner.hpp"
#include "fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::compress {
namespace {

using namespace std::chrono_literals;
using testing::FakeProbe;

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t mib = kib * kib;
const WallTime start = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};
const WallTime old = start - std::chrono::days{90};

space::FileItem item(const std::string& name, std::uint64_t size, WallTime modified = old) {
  return {.name = name,
          .size = Bytes{size},
          .size_on_disk = Bytes{(size + 4095) / 4096 * 4096},
          .modified = modified};
}

// One folder `root` with `files`, and a subfolder "sub" with `sub_files`.
space::ScanResult scan_of(const std::filesystem::path& root, std::vector<space::FileItem> files,
                          std::vector<space::FileItem> sub_files = {}) {
  space::ScanResult scan;
  scan.folders.push_back({.path = root, .folders = {1}, .files = std::move(files)});
  scan.folders.push_back({.path = root / "sub", .parent = 0, .files = std::move(sub_files)});
  for (const space::Folder& folder : scan.folders) {
    for (const space::FileItem& file : folder.files) {
      scan.folders[0].size_on_disk += file.size_on_disk;
    }
  }
  return scan;
}

std::vector<space::FileItem> many(const std::string& extension, std::size_t count,
                                  std::uint64_t size, WallTime modified = old) {
  std::vector<space::FileItem> files;
  files.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    files.push_back(item("f" + std::to_string(index) + extension, size, modified));
  }
  return files;
}

std::uint64_t bytes_read(const FakeProbe& probe) {
  std::uint64_t total = 0;
  for (const FakeProbe::Read& read : probe.reads) {
    total += read.length;
  }
  return total;
}

const ExclusionTotal* excluded(const FolderEstimate& result, Exclusion reason) {
  const auto found = std::ranges::find(result.excluded, reason, &ExclusionTotal::reason);
  return found == result.excluded.end() ? nullptr : &*found;
}

TEST(Analyzer, CountsEveryExclusionAndNeverReadsExcludedFiles) {
  const std::filesystem::path root = R"(D:\Data)";
  space::FileItem link = item("link", mib);
  link.link = true;
  space::FileItem cloud = item("cloud.txt", mib);
  cloud.placeholder = true;
  space::FileItem second = item("second.txt", mib);
  second.extra_link = true;
  space::FileItem compressed = item("packed.dll", mib);
  compressed.size_on_disk = Bytes{mib / 2};
  space::ScanResult scan =
      scan_of(root, {link, cloud, second, compressed, item("tiny.txt", 4096),
                     item("photo.JPG", mib), item("disk.vhdx", mib), item("app.sqlite", mib),
                     item("trace.log", mib), item("driver.sys", mib),
                     item("fresh.txt", mib, start - std::chrono::days{3}), item("keep.dll", mib)});
  const fakes::FakeClock clock{start};
  const FakeProbe probe;
  AnalyzerOptions options;
  options.rules.windows = R"(D:\Data\sub)";
  scan.folders[1].files = {item("kernel.dll", mib)};

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  EXPECT_EQ(result.eligible_files, 1U);
  EXPECT_EQ(result.eligible, Bytes{mib});
  for (const Exclusion reason :
       {Exclusion::link, Exclusion::placeholder, Exclusion::extra_link, Exclusion::small,
        Exclusion::compressed, Exclusion::media, Exclusion::disk_image, Exclusion::system_file,
        Exclusion::recent, Exclusion::windows}) {
    const ExclusionTotal* total = excluded(result, reason);
    ASSERT_NE(total, nullptr) << to_string(reason);
    EXPECT_EQ(total->files, 1U) << to_string(reason);
  }
  EXPECT_EQ(excluded(result, Exclusion::database)->files, 2U);
  EXPECT_EQ(excluded(result, Exclusion::extra_link)->size_on_disk, Bytes{});
  for (const FakeProbe::Read& read : probe.reads) {
    EXPECT_EQ(read.file, root / "keep.dll");
  }
}

TEST(Analyzer, ExtrapolatesThePooledRatioPerFileWithChunkTableAndClusters) {
  const space::ScanResult scan = scan_of(R"(D:\Tools)", many(".dll", 100, mib));
  const fakes::FakeClock clock{start};
  const FakeProbe probe;
  AnalyzerOptions options;
  options.budget = Bytes{8 * mib};

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  EXPECT_EQ(result.samples, 128U);
  EXPECT_LE(bytes_read(probe), 8 * mib);
  EXPECT_EQ(result.sampled, Bytes{8 * mib});
  // Half of 1 MiB plus 255 chunk offsets of 4 bytes, rounded up to 4 KiB: 528384 bytes.
  const AlgorithmEstimate* xpress4k = result.find(Algorithm::xpress4k);
  ASSERT_NE(xpress4k, nullptr);
  EXPECT_EQ(xpress4k->savings, Bytes{100 * (mib - 528384)});
  EXPECT_EQ(xpress4k->savings_low, xpress4k->savings);
  EXPECT_EQ(xpress4k->savings_high, xpress4k->savings);
  EXPECT_DOUBLE_EQ(xpress4k->confidence, 1.0);
  // LZX: 1.12 times what XPRESS-32K saves (ratio 0.425): ratio 0.356; modelled, so less sure.
  const AlgorithmEstimate* lzx = result.find(Algorithm::lzx);
  ASSERT_NE(lzx, nullptr);
  EXPECT_GT(lzx->savings, result.find(Algorithm::xpress16k)->savings);
  EXPECT_NEAR(static_cast<double>(lzx->savings.count()) / (100.0 * mib), 0.644, 0.005);
  EXPECT_DOUBLE_EQ(lzx->confidence, 1.0 - lzx_model_error);
  EXPECT_LT(lzx->savings_low, lzx->savings);
  ASSERT_NE(result.find(Algorithm::lznt1), nullptr);
}

TEST(Analyzer, SpreadsSamplesOverEveryFileInProportionToSize) {
  std::vector<space::FileItem> files = many(".dll", 10, mib);
  files.push_back(item("big.bin", 90 * mib));
  const space::ScanResult scan = scan_of(R"(D:\Tools)", files);
  const fakes::FakeClock clock{start};
  FakeProbe probe;
  AnalyzerOptions options;
  options.budget = Bytes{6400 * kib};  // 100 windows over 100 MiB

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  ASSERT_EQ(result.samples, 100U);
  const auto big = std::ranges::count(probe.reads, std::filesystem::path{R"(D:\Tools\big.bin)"},
                                      &FakeProbe::Read::file);
  EXPECT_EQ(big, 90);
}

TEST(Analyzer, ReadsEveryByteWhenTheBudgetCoversTheFolder) {
  const space::ScanResult scan =
      scan_of(R"(D:\Tools)", many(".dll", 3, 100 * kib), many(".exe", 2, 10 * kib));
  const fakes::FakeClock clock{start};
  const FakeProbe probe;

  const FolderEstimate result = estimate(scan, 0, probe, clock);

  EXPECT_EQ(result.sampled, Bytes{320 * kib});
  EXPECT_EQ(result.samples, 8U);  // two windows per 100 KiB file, one per 10 KiB file
  EXPECT_DOUBLE_EQ(result.find(Algorithm::xpress8k)->confidence, 1.0);
}

TEST(Analyzer, VaryingSamplesWidenTheInterval) {
  const space::ScanResult scan = scan_of(R"(D:\Tools)", many(".dll", 200, mib));
  const fakes::FakeClock clock{start};
  FakeProbe probe;
  for (std::size_t index = 0; index < 200; index += 2) {
    probe.ratios[std::filesystem::path{R"(D:\Tools)"} / ("f" + std::to_string(index) + ".dll")] =
        0.2;
  }
  probe.ratio = 0.8;
  AnalyzerOptions options;
  options.budget = Bytes{4 * mib};

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  const AlgorithmEstimate* xpress4k = result.find(Algorithm::xpress4k);
  EXPECT_GT(xpress4k->confidence, 0.0);
  EXPECT_LT(xpress4k->confidence, 1.0);
  EXPECT_LT(xpress4k->savings_low, xpress4k->savings);
  EXPECT_GT(xpress4k->savings_high, xpress4k->savings);
}

TEST(Analyzer, LeavesOutExtensionsMeasuredIncompressible) {
  std::vector<space::FileItem> files = many(".dll", 20, mib);
  for (std::size_t index = 0; index < 5; ++index) {
    files.push_back(item("data" + std::to_string(index) + ".pak", 4 * mib));
  }
  const space::ScanResult scan = scan_of(R"(D:\Game)", files);
  const fakes::FakeClock clock{start};
  FakeProbe probe;
  for (std::size_t index = 0; index < 5; ++index) {
    probe.ratios[std::filesystem::path{R"(D:\Game)"} / ("data" + std::to_string(index) + ".pak")] =
        1.2;
  }
  AnalyzerOptions options;
  options.budget = Bytes{4 * mib};

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  EXPECT_EQ(result.incompressible, std::vector<std::string>{".pak"});
  EXPECT_EQ(excluded(result, Exclusion::incompressible)->files, 5U);
  EXPECT_EQ(result.eligible_files, 20U);
  EXPECT_EQ(result.find(Algorithm::xpress4k)->savings, Bytes{20 * (mib - 528384)});
}

TEST(Analyzer, RecordsUnreadableSamplesAndGoesOn) {
  const space::ScanResult scan = scan_of(R"(D:\Tools)", many(".dll", 4, 64 * kib));
  const fakes::FakeClock clock{start};
  FakeProbe probe;
  probe.failing.insert(R"(D:\Tools\f1.dll)");

  const FolderEstimate result = estimate(scan, 0, probe, clock);

  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_EQ(result.errors[0].path, std::filesystem::path{R"(D:\Tools\f1.dll)"});
  EXPECT_EQ(result.samples, 3U);
}

TEST(Analyzer, StopsAtTheTimeBudgetWithSamplesSpreadOverTheFolder) {
  const space::ScanResult scan = scan_of(R"(D:\Tools)", many(".dll", 64, mib));
  fakes::FakeClock clock{start};
  FakeProbe probe;
  probe.on_sample = [&] { clock.advance(1s); };
  AnalyzerOptions options;
  options.budget = Bytes{4 * mib};
  options.time_budget = 8s;

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  EXPECT_EQ(result.samples, 8U);
  EXPECT_FALSE(result.cancelled);
  // Bit-reversed order: the first eight of 64 windows are files 0, 32, 16, 48, 8, 40, 24, 56.
  EXPECT_EQ(probe.reads.back().file, std::filesystem::path{R"(D:\Tools\f56.dll)"});
}

TEST(Analyzer, StopsWhenCancelled) {
  const space::ScanResult scan = scan_of(R"(D:\Tools)", many(".dll", 8, mib));
  const fakes::FakeClock clock{start};
  FakeProbe probe;
  std::stop_source source;
  probe.on_sample = [&] { source.request_stop(); };

  const FolderEstimate result = estimate(scan, 0, probe, clock, {}, source.get_token());

  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(result.samples, 1U);
}

TEST(Analyzer, RecommendsLzxForToolchainsAndXpress8kForGames) {
  const fakes::FakeClock clock{start};
  const FakeProbe probe;

  const space::ScanResult toolchain =
      scan_of(R"(C:\Users\owner\.rustup\toolchains)", many(".dll", 200, mib));
  const FolderEstimate tools = estimate(toolchain, 0, probe, clock);
  EXPECT_EQ(tools.category, Category::toolchain);
  EXPECT_EQ(tools.recommended, Algorithm::lzx);

  const space::ScanResult game =
      scan_of(R"(D:\SteamLibrary\steamapps\common\Some Game)", many(".bin", 200, mib));
  const FolderEstimate played = estimate(game, 0, probe, clock);
  EXPECT_EQ(played.category, Category::game);
  EXPECT_EQ(played.recommended, Algorithm::xpress8k);
  EXPECT_NE(played.reason.find("LZX about"), std::string::npos);

  const space::ScanResult data = scan_of(R"(D:\Data)", many(".bin", 200, mib));
  EXPECT_EQ(estimate(data, 0, probe, clock).recommended, Algorithm::xpress4k);
}

TEST(Analyzer, RecommendsNothingForFrequentlyWrittenOrSmallSavings) {
  const fakes::FakeClock clock{start};
  const FakeProbe probe;

  std::vector<space::FileItem> files = many(".dll", 100, mib);
  const std::vector<space::FileItem> fresh = many(".obj", 100, mib, start - std::chrono::days{1});
  files.insert(files.end(), fresh.begin(), fresh.end());
  const FolderEstimate written =
      estimate(scan_of(R"(C:\Program Files\App)", files), 0, probe, clock);
  EXPECT_FALSE(written.recommended);
  EXPECT_NE(written.reason.find("frequently written"), std::string::npos);

  const FolderEstimate small =
      estimate(scan_of(R"(C:\Program Files\App)", many(".dll", 10, mib)), 0, probe, clock);
  EXPECT_FALSE(small.recommended);
  EXPECT_NE(small.reason.find("too small"), std::string::npos);

  const FolderEstimate empty = estimate(scan_of(R"(C:\Empty)", {}), 0, probe, clock);
  EXPECT_FALSE(empty.recommended);
  EXPECT_EQ(empty.find(Algorithm::xpress4k)->savings, Bytes{});
}

TEST(Analyzer, CoversSubfoldersAndOnlyTheChosenFolder) {
  const space::ScanResult scan = scan_of(R"(D:\Tools)", many(".dll", 2, mib), many(".exe", 3, mib));
  const fakes::FakeClock clock{start};
  const FakeProbe probe;

  EXPECT_EQ(estimate(scan, 0, probe, clock).eligible_files, 5U);
  EXPECT_EQ(estimate(scan, 1, probe, clock).eligible_files, 3U);
}

class ToolsEnvironment final : public clean::IEnvironment {
 public:
  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view name) const override {
    return name == L"TOOLS" ? std::optional<std::wstring>(LR"(D:\tools)") : std::nullopt;
  }
};

// A folder inside a knowledge pack's compression target gets the target's algorithm, and the
// names the target excludes are left alone.
TEST(Analyzer, FollowsTheCompressionTargetOfAPack) {
  std::vector<space::FileItem> files = many(".dll", 200, mib);
  files.push_back(item("debug.pdb", mib));
  const space::ScanResult scan = scan_of(R"(D:\tools\rust\toolchains\stable)", files);
  const fakes::FakeClock clock{start};
  const FakeProbe probe;
  const ToolsEnvironment environment;
  AnalyzerOptions options;
  options.targets = {packs::CompressTarget{.id = "rust/toolchains",
                                           .label = "Rust toolchains",
                                           .paths = {R"(%TOOLS%\rust\toolchains\*)"},
                                           .algorithm = "lzx",
                                           .exclude = {"*.pdb"}}};
  options.environment = &environment;

  const FolderEstimate result = estimate(scan, 0, probe, clock, options);

  EXPECT_EQ(result.target, "rust/toolchains");
  ASSERT_NE(excluded(result, Exclusion::pack_excluded), nullptr);
  EXPECT_EQ(excluded(result, Exclusion::pack_excluded)->files, 1U);
  ASSERT_TRUE(result.recommended) << result.reason;
  EXPECT_EQ(*result.recommended, Algorithm::lzx);
  EXPECT_NE(result.reason.find("LZX: knowledge pack case rust/toolchains"), std::string::npos)
      << result.reason;

  options.environment = nullptr;  // no environment: targets are not used
  const FolderEstimate plain = estimate(scan, 0, probe, clock, options);
  EXPECT_TRUE(plain.target.empty());
  EXPECT_EQ(excluded(plain, Exclusion::pack_excluded), nullptr);
}

}  // namespace
}  // namespace myclean::compress
