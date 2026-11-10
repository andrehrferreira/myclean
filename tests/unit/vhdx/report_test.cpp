#include "core/vhdx/report.hpp"

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "clean/clean_fakes.hpp"
#include "core/map/attribution.hpp"
#include "core/model/units.hpp"

namespace myclean::vhdx {
namespace {

namespace fs = std::filesystem;
using namespace myclean::literals;

TEST(VhdxReport, KnowsADiskByItsExtension) {
  EXPECT_TRUE(is_vhdx(L"D:\\wsl\\ext4.vhdx"));
  EXPECT_TRUE(is_vhdx(L"D:\\wsl\\EXT4.VHDX"));
  EXPECT_FALSE(is_vhdx(L"D:\\wsl\\ext4.vhd"));
  EXPECT_FALSE(is_vhdx(L"D:\\wsl\\ext4.vhdx.bak"));
}

TEST(VhdxReport, ListsEveryDiskOfItsSourcesOnceLargestFirst) {
  clean::testing::FakeTree tree;
  clean::testing::FakeUsage usage;
  tree.add_file(L"D:\\Database\\DockerDesktopWSL\\disk\\docker_data.vhdx", 1500_GiB, {});
  tree.add_file(L"D:\\Database\\DockerDesktopWSL\\main\\ext4.vhdx", 104_MiB, {});
  tree.add_file(L"D:\\Database\\DockerDesktopWSL\\main\\deep\\er\\old.vhdx", 1_GiB, {});
  tree.add_file(L"D:\\Database\\DockerDesktopWSL\\notes.txt", 1_KiB, {});
  tree.add_file(L"C:\\Users\\o\\AppData\\Local\\Packages\\Ubuntu\\LocalState\\ext4.vhdx", 120_GiB,
                {});
  tree.add_file(L"C:\\cloud\\ext4.vhdx", 9_GiB, {}, {.recall_on_data_access = true});
  usage.lock(L"D:\\Database\\DockerDesktopWSL\\disk\\docker_data.vhdx");
  const std::vector<DiskSource> sources = {
      {.location = L"C:\\Users\\o\\AppData\\Local\\Packages\\Ubuntu\\LocalState",
       .area = "wsl-docker/distro",
       .label = "WSL distribution disk (ext4.vhdx)",
       .distribution = "Ubuntu-24.04"},
      {.location = L"D:\\Database\\DockerDesktopWSL",
       .area = "wsl-docker/docker-data-custom",
       .label = "Docker Desktop disk"},
      // The same distribution disk again, through a pattern: listed once, with its name.
      {.location = L"C:\\Users\\o\\AppData\\Local\\Packages\\Ubuntu\\LocalState\\ext4.vhdx",
       .area = "wsl-docker/distro-disks",
       .label = "WSL Linux distribution disks"},
      {.location = L"C:\\cloud\\ext4.vhdx", .area = "x/cloud", .label = "cloud"},
      {.location = L"E:\\missing", .area = "x/missing", .label = "missing"}};

  const std::vector<VirtualDisk> disks = find_disks(tree, sources, &usage);

  ASSERT_EQ(disks.size(), 3U);
  EXPECT_EQ(disks[0].path, fs::path{L"D:\\Database\\DockerDesktopWSL\\disk\\docker_data.vhdx"});
  EXPECT_EQ(disks[0].size, 1500_GiB);
  EXPECT_TRUE(disks[0].in_use);
  EXPECT_EQ(disks[1].distribution, "Ubuntu-24.04");
  EXPECT_EQ(disks[1].area, "wsl-docker/distro");
  EXPECT_EQ(disks[1].size, 120_GiB);
  EXPECT_FALSE(disks[1].in_use);
  EXPECT_EQ(disks[2].path, fs::path{L"D:\\Database\\DockerDesktopWSL\\main\\ext4.vhdx"});

  const nlohmann::json json = disks[1];
  EXPECT_EQ(json.at("path"),
            "C:\\Users\\o\\AppData\\Local\\Packages\\Ubuntu\\LocalState\\ext4.vhdx");
  EXPECT_EQ(json.at("size"), 120ULL << 30U);
  EXPECT_EQ(json.at("distribution"), "Ubuntu-24.04");
  EXPECT_EQ(nlohmann::json(disks[0]).at("distribution"), nullptr);
}

TEST(VhdxReport, ResolvesTheVmDiskAreasForTheOwner) {
  clean::testing::FakeTree tree;
  clean::testing::FakeEnvironment environment;
  environment.set(L"LOCALAPPDATA", L"C:\\Users\\o\\AppData\\Local");
  tree.add_file(L"C:\\Users\\o\\AppData\\Local\\Packages\\Ubuntu\\LocalState\\ext4.vhdx", 1_GiB,
                {});
  tree.add_file(L"C:\\Users\\o\\AppData\\Local\\Docker\\wsl\\disk\\docker_data.vhdx", 2_GiB, {});
  tree.add_file(L"C:\\Users\\o\\AppData\\Local\\npm-cache\\x", 1_GiB, {});
  const std::vector<map::Area> areas = {
      {.id = "wsl-docker/distro-disks",
       .paths = {L"%LOCALAPPDATA%\\Packages\\*\\LocalState\\ext4.vhdx"},
       .category = map::Category::vm_disks,
       .label = "WSL Linux distribution disks",
       .reclaim = {}},
      {.id = "wsl-docker/docker-data",
       .paths = {L"%LOCALAPPDATA%\\Docker\\wsl"},
       .category = map::Category::vm_disks,
       .label = "Docker Desktop disk",
       .reclaim = {}},
      {.id = "node/npm-cache",
       .paths = {L"%LOCALAPPDATA%\\npm-cache"},
       .category = map::Category::dev_caches,
       .label = "npm cache",
       .reclaim = {}}};

  const std::vector<DiskSource> sources = area_sources(tree, areas, environment);
  ASSERT_EQ(sources.size(), 2U);
  EXPECT_EQ(sources[0].area, "wsl-docker/distro-disks");
  EXPECT_EQ(sources[1].location, fs::path{L"C:\\Users\\o\\AppData\\Local\\Docker\\wsl"});

  const std::vector<VirtualDisk> disks = find_disks(tree, sources, nullptr);
  ASSERT_EQ(disks.size(), 2U);
  EXPECT_EQ(disks[0].label, "Docker Desktop disk");
}

}  // namespace
}  // namespace myclean::vhdx
