#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/compress/compact_os.hpp"
#include "core/compress/compress_folder.hpp"
#include "core/compress/compression.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::compress {
namespace {

using testing::FakeReader;
using testing::FakeRunner;
using testing::FakeWof;
using testing::identity_of;

constexpr std::uint64_t mib = std::uint64_t{1} << 20U;
const WallTime start = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};
const WallTime old = start - std::chrono::days{90};
const std::filesystem::path root = R"(D:\Tools)";

space::ListedEntry entry(const std::string& name, std::uint8_t id, std::uint64_t size = mib,
                         WallTime modified = old) {
  return {.name = name,
          .size = Bytes{size},
          .size_on_disk = Bytes{(size + 4095) / 4096 * 4096},
          .modified = modified,
          .identity = identity_of(id)};
}

// A folder of `count` eligible 1 MiB files f0.dll.. with ids 1.., known to `wof` too.
void eligible_files(FakeReader& reader, FakeWof& wof, int count) {
  for (int index = 0; index < count; ++index) {
    const std::string name = "f" + std::to_string(index) + ".dll";
    const auto id = static_cast<std::uint8_t>(index + 1);
    reader.folders[root].push_back(entry(name, id));
    wof.add(root / name, identity_of(id), Bytes{mib});
  }
}

std::vector<std::filesystem::path> paths(const Preview& preview) {
  std::vector<std::filesystem::path> result;
  result.reserve(preview.items.size());
  for (const PlanItem& item : preview.items) {
    result.push_back(item.path);
  }
  return result;
}

TEST(CompressFolder, DescribesAnAskActionOfTheService) {
  const FakeReader reader;
  FakeWof wof;
  const fakes::FakeClock clock{start};
  const CompressFolder action{reader, wof, clock, root, Algorithm::lzx, {}};

  const ActionInfo info = action.describe();
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.context, ExecutionContext::service);
  EXPECT_NE(info.what.find("lzx"), std::string::npos);
}

TEST(CompressFolder, PreviewsOnlyTheFilesTheRulesAllow) {
  FakeReader reader;
  const space::ListedEntry lib{.name = "lib", .directory = true, .identity = identity_of(9)};
  reader.folders[root] = {entry("a.dll", 1), entry("b.jpg", 2),
                          entry("c.dll", 3, mib, start - std::chrono::days{2}),
                          entry("small.txt", 4, 100), lib};
  reader.folders[root / "lib"] = {entry("d.dll", 5, 2 * mib)};
  FakeWof wof;
  const fakes::FakeClock clock{start};
  CompressFolder action{reader, wof, clock, root, Algorithm::xpress8k, {}};

  const Result<Preview> preview = action.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  EXPECT_EQ(paths(*preview),
            (std::vector<std::filesystem::path>{root / "a.dll", root / "lib" / "d.dll"}));
  EXPECT_EQ(preview->items[0].id, PlanItemId{1});
  EXPECT_EQ(preview->items[1].id, PlanItemId{2});
  EXPECT_EQ(preview->items[1].identity, identity_of(5));
  EXPECT_EQ(preview->total_size(), Bytes{3 * mib});
}

TEST(CompressFolder, NeverPreviewsTheFilesOfAWslDistribution) {
  const std::filesystem::path package =
      R"(C:\Users\owner\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_79rhkp1fndgsc)";
  const std::filesystem::path state = package / "LocalState";
  FakeReader reader;
  reader.folders[package] = {{.name = "LocalState", .directory = true, .identity = identity_of(8)}};
  reader.folders[state] = {entry("ext4.vhdx", 1),
                           {.name = "rootfs", .directory = true, .identity = identity_of(9)},
                           entry("settings.dat", 2)};
  reader.folders[state / "rootfs"] = {
      {.name = "usr", .directory = true, .identity = identity_of(10)}};
  reader.folders[state / "rootfs" / "usr"] = {entry("libc.so.6", 3)};
  FakeWof wof;
  const fakes::FakeClock clock{start};
  CompressFolder action{reader, wof, clock, package, Algorithm::xpress8k, {}};

  const Result<Preview> preview = action.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  EXPECT_EQ(paths(*preview), (std::vector<std::filesystem::path>{state / "settings.dat"}));
}

TEST(CompressFolder, RefusesLznt1) {
  const FakeReader reader;
  FakeWof wof;
  const fakes::FakeClock clock{start};
  CompressFolder action{reader, wof, clock, root, Algorithm::lznt1, {}};

  const Result<Preview> preview = action.preview();
  ASSERT_FALSE(preview);
  EXPECT_EQ(preview.error().code, Errc::invalid_argument);
}

TEST(CompressFolder, AppliesAndReportsEveryOutcome) {
  FakeReader reader;
  FakeWof wof;
  eligible_files(reader, wof, 5);
  wof.in_use.insert(root / "f1.dll");
  wof.not_beneficial.insert(root / "f2.dll");
  wof.files.at(root / "f3.dll").identity = identity_of(42);  // replaced since the preview
  wof.files.erase(root / "f4.dll");                          // removed since the preview
  const fakes::FakeClock clock{start};
  CompressFolder action{reader, wof, clock, root, Algorithm::xpress4k, {}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = action.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_TRUE(undo->reversible());
  ASSERT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(undo->items[0].item, PlanItemId{1});
  EXPECT_EQ(std::filesystem::path{undo->items[0].data}, root / "f0.dll");
  EXPECT_EQ(wof.files.at(root / "f0.dll").wof, Algorithm::xpress4k);
  const CompressReport& report = action.report();
  EXPECT_EQ(report.compressed, 1U);
  EXPECT_EQ(report.in_use, 1U);
  EXPECT_EQ(report.not_beneficial, 1U);
  EXPECT_EQ(report.changed, 2U);
  EXPECT_FALSE(report.stopped);
  EXPECT_TRUE(report.errors.empty());
  // The replaced file was never handed to WOF.
  EXPECT_EQ(wof.compressed, (std::vector<std::filesystem::path>{root / "f0.dll", root / "f1.dll",
                                                                root / "f2.dll"}));
}

TEST(CompressFolder, StopsAtTheByteBudgetAndResumesWhereItStopped) {
  FakeReader reader;
  FakeWof wof;
  eligible_files(reader, wof, 4);
  const fakes::FakeClock clock{start};
  CompressFolder first{reader, wof, clock, root, Algorithm::lzx, {.byte_budget = Bytes{2 * mib}}};
  const Result<Preview> preview = first.preview();
  ASSERT_TRUE(preview);

  ASSERT_TRUE(first.apply(*preview));
  EXPECT_EQ(first.report().compressed, 2U);
  EXPECT_TRUE(first.report().stopped);

  CompressFolder second{reader, wof, clock, root, Algorithm::lzx, {}};
  const Result<Undo> undo = second.apply(*preview);
  ASSERT_TRUE(undo);
  EXPECT_EQ(second.report().already, 2U);
  EXPECT_EQ(second.report().compressed, 2U);
  EXPECT_EQ(undo->items.size(), 2U);
}

TEST(CompressFolder, StopsWhenCancelled) {
  FakeReader reader;
  FakeWof wof;
  eligible_files(reader, wof, 3);
  const fakes::FakeClock clock{start};
  std::stop_source source;
  CompressFolder action{reader, wof, clock, root, Algorithm::xpress4k, {}, source.get_token()};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  source.request_stop();

  const Result<Undo> undo = action.apply(*preview);

  ASSERT_TRUE(undo);
  EXPECT_TRUE(undo->items.empty());
  EXPECT_TRUE(action.report().stopped);
  EXPECT_TRUE(wof.compressed.empty());
}

TEST(CompressFolder, VerifiesTheSmallerSizeOnDiskAndUndoesExactly) {
  FakeReader reader;
  FakeWof wof;
  eligible_files(reader, wof, 2);
  const fakes::FakeClock clock{start};
  CompressFolder action{reader, wof, clock, root, Algorithm::xpress16k, {}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo);

  const Result<Verification> verification = action.verify(*undo);
  ASSERT_TRUE(verification) << verification.error().message;
  EXPECT_EQ(verification->metric, "size_on_disk_bytes");
  EXPECT_DOUBLE_EQ(verification->before, 2.0 * mib);
  EXPECT_DOUBLE_EQ(verification->after, 1.0 * mib);
  EXPECT_EQ(verification->effect, Effect::better);

  ASSERT_TRUE(action.undo(*undo));
  for (const auto& [path, state] : wof.files) {
    EXPECT_FALSE(state.wof) << path;
    EXPECT_EQ(state.size_on_disk, Bytes{mib}) << path;
  }
  EXPECT_EQ(wof.decompressed.size(), 2U);
  ASSERT_TRUE(action.undo(*undo));  // nothing left to decompress
  EXPECT_EQ(wof.decompressed.size(), 2U);
}

TEST(CompressFolder, UndoDecompressesTheRestAndReportsFailures) {
  FakeReader reader;
  FakeWof wof;
  eligible_files(reader, wof, 3);
  const fakes::FakeClock clock{start};
  CompressFolder action{reader, wof, clock, root, Algorithm::xpress4k, {}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo);
  wof.failures.insert_or_assign(root / "f1.dll",
                                Error{.code = Errc::io_error, .message = "disk full"});

  const Result<void> undone = action.undo(*undo);

  ASSERT_FALSE(undone);
  EXPECT_EQ(undone.error().code, Errc::io_error);
  EXPECT_NE(undone.error().message.find("1 of 3"), std::string::npos);
  EXPECT_NE(undone.error().message.find("disk full"), std::string::npos);
  EXPECT_FALSE(wof.files.at(root / "f0.dll").wof);
  EXPECT_FALSE(wof.files.at(root / "f2.dll").wof);
}

constexpr const char* compact = R"(C:\Windows\System32\compact.exe)";

TEST(CompactOs, PreviewsNothingWhenItIsOnAlready) {
  FakeRunner runner;
  FakeWof wof;
  wof.compact_os_on = true;
  CompactOs action{runner, wof, compact, Bytes{3 * mib}};

  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  EXPECT_TRUE(preview->items.empty());
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo);
  EXPECT_TRUE(runner.calls.empty());
  EXPECT_EQ(action.verify(*undo)->effect, Effect::unchanged);
  EXPECT_EQ(action.describe().tier, Tier::ask);
}

TEST(CompactOs, TurnsItOnJudgedByTheStateAndOffAgain) {
  FakeRunner runner;
  FakeWof wof;
  runner.on_run = [&](const std::string& line) { wof.compact_os_on = line.ends_with("always"); };
  CompactOs action{runner, wof, compact, Bytes{3 * mib}};

  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, compact_os_item());
  EXPECT_EQ(preview->items[0].size, Bytes{3 * mib});
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(runner.calls, std::vector<std::string>{std::string(compact) + " /compactos:always"});
  const Result<Verification> verification = action.verify(*undo);
  ASSERT_TRUE(verification);
  EXPECT_EQ(verification->effect, Effect::better);

  ASSERT_TRUE(action.undo(*undo));
  EXPECT_EQ(runner.calls.back(), std::string(compact) + " /compactos:never");
  EXPECT_FALSE(wof.compact_os_on);
}

TEST(CompactOs, FailsWhenTheStateDidNotChange) {
  FakeRunner runner;
  runner.output = {.exit_code = 5, .out = "Acesso negado.\r\n"};
  const FakeWof wof;
  CompactOs action{runner, wof, compact, Bytes{}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = action.apply(*preview);

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::io_error);
  EXPECT_NE(undo.error().message.find("exit code 5"), std::string::npos);
  EXPECT_NE(undo.error().message.find("Acesso negado."), std::string::npos);
}

}  // namespace
}  // namespace myclean::compress
