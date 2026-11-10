#include "core/vhdx/compact.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "fakes/fake_file_system.hpp"
#include "vhdx_fakes.hpp"

namespace myclean::vhdx {
namespace {

namespace fs = std::filesystem;
using namespace myclean::literals;

const fs::path docker = L"D:\\Database\\DockerDesktopWSL\\disk\\docker_data.vhdx";
const fs::path ubuntu = L"C:\\Users\\o\\AppData\\Local\\Packages\\Ubuntu\\LocalState\\ext4.vhdx";

// Two disks on a fake file system, a WSL that shuts down and a disk compactor, journalling both.
struct Machine {
  Machine() {
    file_system.add_file(docker, 1500_GiB);
    file_system.add_file(ubuntu, 120_GiB);
    wsl.journal = &journal;
    disks.journal = &journal;
  }
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;
  Machine(Machine&&) = delete;
  Machine& operator=(Machine&&) = delete;
  ~Machine() = default;

  [[nodiscard]] CompactVhdx action(std::vector<fs::path> files) {
    return {file_system, wsl, disks, std::move(files)};
  }

  fakes::FakeFileSystem file_system;
  std::vector<std::string> journal;
  testing::FakeWsl wsl;
  testing::FakeVirtualDisks disks{file_system};
};

class CompactVhdxTest : public ::testing::Test {
 protected:
  [[nodiscard]] CompactVhdx action(std::vector<fs::path> files) {
    return machine_.action(std::move(files));
  }
  fakes::FakeFileSystem& file_system() { return machine_.file_system; }
  std::vector<std::string>& journal() { return machine_.journal; }
  testing::FakeWsl& wsl() { return machine_.wsl; }
  testing::FakeVirtualDisks& disks() { return machine_.disks; }

 private:
  Machine machine_;
};

TEST_F(CompactVhdxTest, IsAnAskTierServiceActionThatSaysEverythingStops) {
  const ActionInfo info = action({docker}).describe();
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.context, ExecutionContext::service);
  EXPECT_NE(info.why.find("stops"), std::string::npos);
}

TEST_F(CompactVhdxTest, PreviewListsEachDiskWithItsSizeAndChangesNothing) {
  CompactVhdx compact = action({docker, ubuntu});
  const Result<Preview> preview = compact.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_EQ(preview->items[0].path, docker);
  EXPECT_EQ(preview->items[0].size, 1500_GiB);
  EXPECT_EQ(preview->items[1].size, 120_GiB);
  EXPECT_TRUE(journal().empty());  // no shutdown, no compaction before approval
}

TEST_F(CompactVhdxTest, PreviewRefusesWhatIsNoVirtualDisk) {
  file_system().add_file(L"D:\\data\\notes.txt", 1_KiB);
  file_system().add_directory(L"D:\\data\\folder.vhdx");
  EXPECT_EQ(action({L"D:\\data\\notes.txt"}).preview().error().code, Errc::invalid_argument);
  EXPECT_EQ(action({L"D:\\data\\folder.vhdx"}).preview().error().code, Errc::invalid_argument);
  EXPECT_EQ(action({docker, docker}).preview().error().code, Errc::invalid_argument);
  EXPECT_EQ(action({L"E:\\gone.vhdx"}).preview().error().code, Errc::not_found);
}

TEST_F(CompactVhdxTest, ApplyShutsWslDownFirstThenCompactsAndRecordsTheSizes) {
  CompactVhdx compact = action({docker, ubuntu});
  const Preview preview = compact.preview().value();
  disks().after[docker] = 900_GiB;
  disks().after[ubuntu] = 40_GiB;

  const Result<Undo> undo = compact.apply(preview);

  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(journal(), (std::vector<std::string>{"shutdown", "compact docker_data.vhdx",
                                                 "compact ext4.vhdx"}));
  EXPECT_FALSE(undo->reversible());
  ASSERT_EQ(undo->items.size(), 2U);
  const nlohmann::json first = nlohmann::json::parse(undo->items[0].data);
  EXPECT_EQ(first.at("before"), 1500ULL << 30U);
  EXPECT_EQ(first.at("after"), 900ULL << 30U);

  const Result<Verification> verified = compact.verify(*undo);
  ASSERT_TRUE(verified.has_value()) << verified.error().message;
  EXPECT_EQ(verified->metric, "vhdx_bytes");
  EXPECT_EQ(verified->before, static_cast<double>((1500ULL + 120ULL) << 30U));
  EXPECT_EQ(verified->after, static_cast<double>((900ULL + 40ULL) << 30U));
  EXPECT_EQ(verified->effect, Effect::better);
  EXPECT_EQ(compact.undo(*undo).error().code, Errc::not_reversible);
}

TEST_F(CompactVhdxTest, AFailedShutdownCompactsNothing) {
  CompactVhdx compact = action({docker});
  const Preview preview = compact.preview().value();
  wsl().error = Error{.code = Errc::io_error, .message = "wsl.exe exited with 1"};

  const Result<Undo> undo = compact.apply(preview);

  ASSERT_FALSE(undo.has_value());
  EXPECT_NE(undo.error().message.find("nothing was compacted"), std::string::npos);
  EXPECT_TRUE(disks().compacted.empty());
}

TEST_F(CompactVhdxTest, ADiskStillInUseIsSkippedAndTheOthersCompacted) {
  CompactVhdx compact = action({docker, ubuntu});
  const Preview preview = compact.preview().value();
  disks().errors.insert_or_assign(docker, Error{.code = Errc::in_use, .message = "attached"});
  disks().after[ubuntu] = 40_GiB;

  const Result<Undo> undo = compact.apply(preview);

  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_TRUE(nlohmann::json::parse(undo->items[0].data).contains("error"));
  const Result<Verification> verified = compact.verify(*undo);
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(verified->before, static_cast<double>(120ULL << 30U));  // only the compacted disk
  EXPECT_EQ(verified->after, static_cast<double>(40ULL << 30U));
}

TEST_F(CompactVhdxTest, WhenNoDiskCouldBeCompactedTheErrorIsReturned) {
  CompactVhdx compact = action({docker});
  const Preview preview = compact.preview().value();
  disks().errors.insert_or_assign(docker, Error{.code = Errc::in_use, .message = "attached"});
  const Result<Undo> undo = compact.apply(preview);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::in_use);
}

TEST_F(CompactVhdxTest, ApplyRefusesAFileSwappedSinceThePreview) {
  CompactVhdx compact = action({docker});
  const Preview preview = compact.preview().value();
  file_system().erase(docker);
  file_system().add_file(docker, 1500_GiB);  // another file at the same path

  const Result<Undo> undo = compact.apply(preview);

  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::refused);
  EXPECT_TRUE(journal().empty());
}

}  // namespace
}  // namespace myclean::vhdx
