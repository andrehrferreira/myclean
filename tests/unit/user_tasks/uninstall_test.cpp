#include "core/user_tasks/uninstall.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../placement/placement_fakes.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "core/user_tasks/task.hpp"

namespace myclean::user_tasks {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

// Outside the test macros: MSVC misreads a raw string that starts with a quote inside one.
const std::string amplitube = R"("C:\Program Files\AmpliTube 5\unins000.exe")";
const std::string quoted = R"("C:\Program Files\X\unins000.exe" /SILENT)";
const std::string empty_quoted = R"("" /x)";
const std::string unclosed = R"("C:\unclosed.exe /x)";
const std::string key = R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{AMP-5})";

// Starts nothing: records each launch, and runs `on_launch` as the uninstaller would.
class FakeLauncher final : public IUninstallLauncher {
 public:
  struct Call {
    fs::path program;
    std::string arguments;
    std::chrono::minutes wait;
  };

  [[nodiscard]] Result<void> launch(const fs::path& program, std::string_view arguments,
                                    std::chrono::minutes wait,
                                    const std::stop_token& /*stop*/) override {
    calls.push_back(Call{.program = program, .arguments = std::string(arguments), .wait = wait});
    if (failure) {
      return fail(*failure, "injected launch failure");
    }
    if (on_launch) {
      on_launch();
    }
    return {};
  }

  std::vector<Call> calls;
  std::optional<Errc> failure;
  std::function<void()> on_launch;
};

class UninstallTest : public ::testing::Test {
 protected:
  UninstallTest() {
    registry_.put(key, "DisplayName", text("AmpliTube 5"));
    registry_.put(key, "UninstallString", text(amplitube));
    registry_.put(key, "EstimatedSize",
                  placement::RegistryValue{.text = "2048", .type = placement::RegistryType::dword});
  }

  static placement::RegistryValue text(std::string value) {
    return placement::RegistryValue{.text = std::move(value)};
  }

  UninstallAction action(std::string name = "AmpliTube 5") {
    return {registry_, launcher_, key, std::move(name), {}};
  }

  // Previews and applies `uninstalling`; the undo data, or the failure.
  static Result<Undo> run(UninstallAction& uninstalling) {
    Result<Preview> preview = uninstalling.preview();
    if (!preview) {
      return tl::unexpected(preview.error());
    }
    preview->items.front().id = PlanItemId{401};
    return uninstalling.apply(*preview);
  }

  placement::testing::FakeRegistry& registry() { return registry_; }
  FakeLauncher& launcher() { return launcher_; }

 private:
  placement::testing::FakeRegistry registry_;
  FakeLauncher launcher_;
};

// Owner-decisions spec, "Launch the uninstaller": the program's own uninstaller runs, and the
// plan says it was launched until its entry is gone.
TEST_F(UninstallTest, StartsTheProgramsOwnUninstallerAndSaysToFinishItThere) {
  UninstallAction uninstalling = action();
  const Result<Preview> preview = uninstalling.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, fs::path("uninstall:AmpliTube 5"));
  EXPECT_EQ(preview->items[0].size, Bytes{std::uint64_t{2048} * 1024});

  const Result<Undo> undo = run(uninstalling);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  ASSERT_EQ(launcher().calls.size(), 1U);
  EXPECT_EQ(launcher().calls[0].program, fs::path(LR"(C:\Program Files\AmpliTube 5\unins000.exe)"));
  EXPECT_EQ(launcher().calls[0].arguments, "");
  EXPECT_EQ(launcher().calls[0].wait, uninstall_wait);
  EXPECT_FALSE(undo->reversible());
  ASSERT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(Json::parse(undo->items[0].data).at("command"), amplitube);

  const Result<Verification> verified = uninstalling.verify(*undo);
  ASSERT_FALSE(verified.has_value());
  EXPECT_NE(verified.error().message.find("finish in the uninstaller"), std::string::npos);
  EXPECT_NE(verified.error().message.find("unins000.exe"), std::string::npos);
  EXPECT_EQ(uninstalling.undo(*undo).error().code, Errc::not_reversible);
}

TEST_F(UninstallTest, AnEntryGoneAfterTheUninstallerEndedIsVerified) {
  launcher().on_launch = [this] { registry().drop(key); };
  UninstallAction uninstalling = action();
  const Result<Undo> undo = run(uninstalling);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  const Result<Verification> verified = uninstalling.verify(*undo);
  ASSERT_TRUE(verified.has_value()) << verified.error().message;
  EXPECT_EQ(verified->metric, "uninstall_entry");
  EXPECT_EQ(verified->effect, Effect::better);
}

TEST_F(UninstallTest, AProgramGoneOrReplacedIsNotUninstalled) {
  UninstallAction other = action("Another Program");
  EXPECT_EQ(run(other).error().code, Errc::refused);
  registry().drop(key);
  UninstallAction gone = action();
  EXPECT_EQ(run(gone).error().code, Errc::not_found);
  EXPECT_TRUE(launcher().calls.empty());
}

TEST_F(UninstallTest, ADeclinedElevationPromptFails) {
  launcher().failure = Errc::refused;
  UninstallAction uninstalling = action();
  const Result<Undo> undo = run(uninstalling);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::refused);
}

TEST(SplitCommandTest, SplitsTheUninstallStringsInstallersWrite) {
  EXPECT_EQ(
      split_command(quoted).value(),
      (CommandLine{.program = LR"(C:\Program Files\X\unins000.exe)", .arguments = "/SILENT"}));
  EXPECT_EQ(split_command(R"(C:\Program Files\NVIDIA\Nsight 2023\uninstall.EXE /S /D=x)").value(),
            (CommandLine{.program = LR"(C:\Program Files\NVIDIA\Nsight 2023\uninstall.EXE)",
                         .arguments = "/S /D=x"}));
  EXPECT_EQ(split_command("MsiExec.exe /X{6D1A6B7E-0C59-4E52-9C2C-7A1F0F3B9E11}").value(),
            (CommandLine{.program = L"MsiExec.exe",
                         .arguments = "/X{6D1A6B7E-0C59-4E52-9C2C-7A1F0F3B9E11}"}));
  EXPECT_EQ(split_command(R"(C:\Tools\my.exe.files\un.exe)").value(),
            (CommandLine{.program = LR"(C:\Tools\my.exe.files\un.exe)", .arguments = ""}));
  EXPECT_EQ(split_command("rundll32 shell32.dll,Control_RunDLL").value(),
            (CommandLine{.program = L"rundll32", .arguments = "shell32.dll,Control_RunDLL"}));
  EXPECT_EQ(split_command("   ").error().code, Errc::invalid_argument);
  EXPECT_EQ(split_command(unclosed).error().code, Errc::invalid_argument);
  EXPECT_EQ(split_command(empty_quoted).error().code, Errc::invalid_argument);
}

// The tray reads an uninstall task of user_tasks.next and reports the launch as applied.
TEST_F(UninstallTest, TheTrayRunsAnUninstallTaskAndReportsTheLaunch) {
  const Json task{{"plan", 40},
                  {"title", "Uninstall AmpliTube 5"},
                  {"kind", "uninstall"},
                  {"operation", "apply"},
                  {"tier", "ask"},
                  {"action", {{"key", key}, {"name", "AmpliTube 5"}}},
                  {"items", Json::array({Json{{"id", 401},
                                              {"path", "uninstall:AmpliTube 5"},
                                              {"bytes", 2048 * 1024},
                                              {"approved", true},
                                              {"volume_serial", 0},
                                              {"file_id", ""}}})}};
  const Result<UserTask> parsed = parse_task(task);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->kind, TaskKind::uninstall);
  EXPECT_EQ(parsed->program_key, key);
  EXPECT_EQ(parsed->program_name, "AmpliTube 5");
  EXPECT_EQ(parsed->items.front().identity, std::nullopt);

  EXPECT_EQ(user_tasks::run(*parsed, Toolbox{}, {}).state, PlanState::failed);
  EXPECT_TRUE(launcher().calls.empty());

  const Toolbox toolbox{.placement = {.registry = &registry()}, .launcher = &launcher()};
  const TaskReport report = user_tasks::run(*parsed, toolbox, {});
  EXPECT_EQ(report.state, PlanState::applied);
  EXPECT_NE(report.error.find("finish in the uninstaller"), std::string::npos) << report.error;
  EXPECT_EQ(launcher().calls.size(), 1U);
  const Json record = report_record(report);
  EXPECT_EQ(record.at("not_reversible"),
            "an uninstalled program comes back only with its installer");

  Json undo = task;
  undo["operation"] = "undo";
  undo["undo"] = {{"not_reversible", "x"}, {"items", Json::array()}};
  const Result<UserTask> undo_task = parse_task(undo);
  ASSERT_TRUE(undo_task.has_value()) << undo_task.error().message;
  EXPECT_EQ(user_tasks::run(*undo_task, toolbox, {}).state, PlanState::failed);
}

}  // namespace
}  // namespace myclean::user_tasks
