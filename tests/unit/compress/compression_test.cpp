#include "core/compress/compression.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

namespace myclean::compress {
namespace {

const WallTime now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};

TEST(Algorithm, HasFixedTextFormsAndChunks) {
  for (const Algorithm algorithm : {Algorithm::xpress4k, Algorithm::xpress8k, Algorithm::xpress16k,
                                    Algorithm::lzx, Algorithm::lznt1}) {
    EXPECT_EQ(parse_algorithm(to_string(algorithm)), algorithm);
  }
  EXPECT_FALSE(parse_algorithm("XPRESS4K"));
  EXPECT_EQ(chunk_bytes(Algorithm::xpress4k), 4096U);
  EXPECT_EQ(chunk_bytes(Algorithm::xpress16k), 16384U);
  EXPECT_EQ(chunk_bytes(Algorithm::lzx), 32768U);
  EXPECT_FALSE(is_wof(Algorithm::lznt1));
  EXPECT_TRUE(is_wof(Algorithm::lzx));
  EXPECT_EQ(to_string(Exclusion::incompressible), "incompressible");
  EXPECT_EQ(to_string(Category::toolchain), "toolchain");
}

TEST(Categorize, RecognizesInstallsByPath) {
  EXPECT_EQ(categorize(R"(C:\Users\owner\.rustup)"), Category::toolchain);
  EXPECT_EQ(categorize(R"(C:\Program Files\Microsoft Visual Studio\2022\Community)"),
            Category::toolchain);
  EXPECT_EQ(categorize(R"(C:\Program Files (x86)\Windows Kits\10)"), Category::toolchain);
  EXPECT_EQ(categorize(R"(C:\Program Files\Epic Games\UE_5.4)"), Category::engine);
  EXPECT_EQ(categorize(R"(C:\Program Files\Unity\Hub\Editor\6000.0.1f1)"), Category::engine);
  EXPECT_EQ(categorize(R"(C:\Program Files\Epic Games\Fortnite)"), Category::game);
  EXPECT_EQ(categorize(R"(E:\SteamLibrary\steamapps\common\Elden Ring)"), Category::game);
  EXPECT_EQ(categorize(R"(C:\XboxGames\Forza)"), Category::game);
  EXPECT_EQ(categorize(R"(C:\Program Files\7-Zip)"), Category::program);
  EXPECT_EQ(categorize(R"(C:\Program Files)"), Category::program);
  EXPECT_EQ(categorize(R"(D:\Photos)"), Category::other);
  EXPECT_EQ(categorize(R"(D:\Rusty\notes)"), Category::other);
}

TEST(IsWithin, ComparesWholeComponentsIgnoringCase) {
  EXPECT_TRUE(is_within(R"(C:\WINDOWS\System32)", R"(C:\Windows)"));
  EXPECT_TRUE(is_within(R"(C:\Windows)", R"(C:\Windows\)"));
  EXPECT_FALSE(is_within(R"(C:\WindowsApps\x)", R"(C:\Windows)"));
  EXPECT_FALSE(is_within(R"(D:\Windows)", R"(C:\Windows)"));
}

TEST(Exclusion, JudgesOneFile) {
  const space::Folder folder{.path = R"(D:\Tools)"};
  const auto file = [](const std::string& name, std::uint64_t size) {
    return space::FileItem{.name = name,
                           .size = Bytes{size},
                           .size_on_disk = Bytes{(size + 4095) / 4096 * 4096},
                           .modified = now - std::chrono::days{31}};
  };
  Rules rules;
  EXPECT_EQ(exclusion(folder, file("a.dll", 5000), rules, now), Exclusion::none);
  EXPECT_EQ(exclusion(folder, file("a.dll", 4096), rules, now), Exclusion::small);
  EXPECT_EQ(exclusion(folder, file("a.MKV", 5000), rules, now), Exclusion::media);
  EXPECT_EQ(exclusion(folder, file("a.db-wal", 5000), rules, now), Exclusion::database);
  EXPECT_EQ(exclusion(folder, file("pagefile.sys", 5000), rules, now), Exclusion::system_file);

  space::FileItem recent = file("a.dll", 5000);
  recent.modified = now - std::chrono::days{29};
  EXPECT_EQ(exclusion(folder, recent, rules, now), Exclusion::recent);
  rules.recent = std::chrono::days{7};
  EXPECT_EQ(exclusion(folder, recent, rules, now), Exclusion::none);

  rules.incompressible = {".pak"};
  EXPECT_EQ(exclusion(folder, file("a.pak", 5000), rules, now), Exclusion::incompressible);
  rules.cluster = Bytes{65536};
  EXPECT_EQ(exclusion(folder, file("a.dll", 5000), rules, now), Exclusion::small);
}

}  // namespace
}  // namespace myclean::compress
