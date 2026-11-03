#include <filesystem>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "core/governor/classifier.hpp"
#include "core/model/result.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/schema.hpp"

// The classes the process-classes pack ships (data/packs/process-classes/classes.toml), loaded
// and merged into a registry as the service does. Pack fixtures have no class kind, so these
// tests are the pack's class fixtures.
namespace myclean::governor {
namespace {

using rules::ProcessClass;

const std::filesystem::path packs_dir = std::filesystem::path{MYCLEAN_DATA_DIR} / "packs";

class ShippedClasses : public ::testing::Test {
 protected:
  void SetUp() override {
    const Result<packs::Pack> pack =
        packs::load_pack(packs::DiskPackFiles{}, packs_dir / "process-classes",
                         packs::Origin::builtin, *packs::parse_version(MYCLEAN_VERSION));
    ASSERT_TRUE(pack.has_value()) << pack.error().message;
    classes_ = packs::make_registry(std::span{&*pack, 1}).classes;
  }

  [[nodiscard]] Classification classify(const std::string& image,
                                        const std::string& command_line = {}) const {
    return governor::classify({.image = image,
                               .image_path = R"(C:\Program Files\Tool\)" + image,
                               .command_line = command_line.empty() ? image : command_line},
                              classes_);
  }

 private:
  rules::ClassSet classes_;
};

TEST_F(ShippedClasses, BuildToolClassifiedByImage) {
  for (const char* image : {"cl.exe", "rustc.exe", "cargo.exe", "link.exe", "ninja.exe",
                            "MSBuild.exe", "build-script-build-1a2b.exe"}) {
    EXPECT_EQ(classify(image).process_class, ProcessClass::build) << image;
  }
  EXPECT_EQ(classify("cl.exe").match_id, "process-classes/msvc");
}

TEST_F(ShippedClasses, CommandLineDecidesTheClass) {
  EXPECT_EQ(classify("node.exe", R"("node.exe" C:\app\node_modules\jest\bin\jest.js --runInBand)")
                .process_class,
            ProcessClass::test);
  EXPECT_EQ(classify("node.exe", R"("node.exe" C:\app\node_modules\vite\bin\vite.js build)")
                .process_class,
            ProcessClass::build);
  EXPECT_EQ(
      classify("node.exe", R"("node.exe" C:\app\node_modules\vitest\vitest.mjs run)").process_class,
      ProcessClass::test);
  EXPECT_EQ(classify("node.exe", R"("node.exe" server.js)").process_class, ProcessClass::normal);
  EXPECT_EQ(classify("python.exe", "python.exe -m pytest -q").process_class, ProcessClass::test);
  EXPECT_EQ(
      classify("java.exe",
               "java.exe -cp gradle.jar org.gradle.launcher.daemon.bootstrap.GradleDaemon 8.10")
          .process_class,
      ProcessClass::build);
  EXPECT_EQ(classify("dotnet.exe", "dotnet.exe test MyApp.sln").process_class, ProcessClass::test);
  EXPECT_EQ(classify("dotnet.exe", "dotnet.exe build MyApp.sln").process_class,
            ProcessClass::build);
}

TEST_F(ShippedClasses, FirstMatchWins) {
  // myclean_governor_tests.exe would match the test entry `*_tests.exe` too; MyClean's own
  // service is listed in `system` above every other entry.
  EXPECT_EQ(classify("myclean-svc.exe").process_class, ProcessClass::system);
  EXPECT_EQ(classify("myclean-tray.exe").process_class, ProcessClass::system);
  EXPECT_EQ(classify("myclean_governor_tests.exe").process_class, ProcessClass::test);
}

TEST_F(ShippedClasses, SystemProcessesAreSystem) {
  for (const char* image : {"svchost.exe", "System Idle Process", "System", "explorer.exe",
                            "dwm.exe", "csrss.exe", "MsMpEng.exe", "conhost.exe"}) {
    EXPECT_EQ(classify(image).process_class, ProcessClass::system) << image;
  }
}

// Observed in the default class on the reference machine, where the ladder could suspend them:
// an antivirus, the WSL VM, WMI, database servers and a remote access tool.
TEST_F(ShippedClasses, ProcessesASuspendWouldBreakAreSystem) {
  for (const char* image : {"avp.exe", "wslservice.exe", "vmmemWSL", "vmwp.exe", "WmiPrvSE.exe",
                            "postgres.exe", "pg_ctl.exe", "mongod.exe", "AnyDesk.exe"}) {
    EXPECT_EQ(classify(image).process_class, ProcessClass::system) << image;
  }
}

TEST_F(ShippedClasses, OrdinaryProgramsAreDefault) {
  for (const char* image : {"notepad.exe", "chrome.exe", "Code.exe", "pwsh.exe"}) {
    EXPECT_EQ(classify(image).process_class, ProcessClass::normal) << image;
  }
}

}  // namespace
}  // namespace myclean::governor
