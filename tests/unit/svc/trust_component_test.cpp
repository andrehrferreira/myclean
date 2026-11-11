#include "svc/trust_component.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/packs/loader.hpp"
#include "core/trust/trust.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

TrustSources sources(Result<std::vector<std::filesystem::path>> roots =
                         std::vector<std::filesystem::path>{LR"(C:\Users\owner)"}) {
  auto inspector = std::make_unique<testing::FakeImageInspector>();
  inspector->signatures = {
      {testing::powertoys_path, trust::Signature{.status = trust::SignatureStatus::valid,
                                                 .signer = "Microsoft Corporation"}},
      {testing::editor_path,
       trust::Signature{.status = trust::SignatureStatus::none, .signer = {}}},
      {LR"(C:\Program Files\Tampered\tool.exe)",
       trust::Signature{.status = trust::SignatureStatus::invalid, .signer = {}}}};
  auto images = std::make_unique<testing::FakeProcessImages>();
  images->paths = {{10, testing::powertoys_path},
                   {20, testing::editor_path},
                   {30, LR"(C:\Program Files\Tampered\tool.exe)"},
                   {40, LR"(C:\Program Files\Gone\gone.exe)"}};
  return TrustSources{.inspector = std::move(inspector),
                      .images = std::move(images),
                      .user_writable_roots = std::move(roots)};
}

class TrustComponentTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(harness_.start(trust_).has_value()); }

  [[nodiscard]] Json signal(std::uint32_t pid) {
    Result<trust::TrustSignal> signal = trust_.process(pid, noon);
    EXPECT_TRUE(signal.has_value()) << signal.error().message;
    return signal ? trust_json(*signal) : Json();
  }

  [[nodiscard]] TrustComponent& trust() { return trust_; }
  [[nodiscard]] testing::FakeImageInspector& inspector() { return *inspector_; }

 private:
  fakes::FakeClock clock_{noon};
  packs::DiskPackFiles files_;
  testing::ComponentHarness harness_{clock_};
  TrustSources sources_ = sources();
  testing::FakeImageInspector* inspector_ =
      dynamic_cast<testing::FakeImageInspector*>(sources_.inspector.get());
  TrustComponent trust_{std::move(sources_), files_};
};

TEST_F(TrustComponentTest, KnownProcessCarriesItsKnowledgeBaseEntry) {
  const Json known = signal(10);
  EXPECT_EQ(known.at("status"), "signed");
  EXPECT_EQ(known.at("signer"), "Microsoft Corporation");
  EXPECT_EQ(known.at("path"), R"(C:\Program Files\PowerToys\PowerToys.exe)");
  EXPECT_EQ(known.at("path_class"), "protected");
  EXPECT_EQ(known.at("unrecognized"), false);
  EXPECT_EQ(known.at("reasons"), Json::array());
  EXPECT_TRUE(known.at("description").get<std::string>().starts_with("Microsoft PowerToys"));
  EXPECT_EQ(known.at("category"), "utility");
  EXPECT_TRUE(trust().health().healthy);
}

TEST_F(TrustComponentTest, UnknownProcessesAreUnrecognizedWithTheirReasons) {
  const Json editor = signal(20);
  EXPECT_EQ(editor.at("status"), "unsigned");
  EXPECT_TRUE(editor.at("signer").is_null());
  EXPECT_EQ(editor.at("path_class"), "user_writable");
  EXPECT_EQ(editor.at("unrecognized"), true);
  EXPECT_EQ(editor.at("reasons"),
            (Json{"not-in-knowledge-base", "unsigned", "user-writable-path"}));
  EXPECT_TRUE(editor.at("description").is_null());
  EXPECT_TRUE(editor.at("category").is_null());

  const Json tampered = signal(30);
  EXPECT_EQ(tampered.at("status"), "invalid");
  EXPECT_EQ(tampered.at("reasons"), (Json{"not-in-knowledge-base", "invalid-signature"}));
  // Facts only: never a verdict.
  for (const Json& value : {editor, tampered}) {
    EXPECT_EQ(value.dump().find("danger"), std::string::npos);
    EXPECT_EQ(value.dump().find("malicious"), std::string::npos);
  }
}

TEST_F(TrustComponentTest, AProcessWhoseImageCannotBeReadHasNoSignal) {
  EXPECT_FALSE(trust().process(40, noon).has_value());  // the file is gone
  EXPECT_FALSE(trust().process(50, noon).has_value());  // the process cannot be opened
}

TEST_F(TrustComponentTest, SignerLookupAnswersFromTheCacheAndVerifiesInTheBackground) {
  const std::string path = R"(C:\Program Files\PowerToys\PowerToys.exe)";
  EXPECT_EQ(trust().signer(path), std::nullopt);  // not seen yet: queued, not verified here
  ASSERT_TRUE(testing::eventually([&] { return trust().signer(path).has_value(); }));
  EXPECT_EQ(trust().signer(path), "Microsoft Corporation");
  EXPECT_EQ(trust().signer(R"(c:\program files\powertoys\POWERTOYS.EXE)"), "Microsoft Corporation");

  const std::string editor = R"(C:\Users\owner\AppData\Local\Editor\editor.exe)";
  static_cast<void>(trust().signer(editor));
  ASSERT_TRUE(testing::eventually([&] {
    const std::scoped_lock lock(inspector().mutex);
    return inspector().verifications == 2;
  }));
  EXPECT_EQ(trust().signer(editor), std::nullopt);  // unsigned

  // The events component backfills the starts stored before: only the signed image, once.
  EXPECT_EQ(trust().take_resolved(),
            (std::vector<std::pair<std::string, std::string>>{{path, "Microsoft Corporation"}}));
  EXPECT_TRUE(trust().take_resolved().empty());

  // A bare file name (a process seeded before events started) is never looked up.
  EXPECT_EQ(trust().signer("PowerToys.exe"), std::nullopt);
  const std::scoped_lock lock(inspector().mutex);
  EXPECT_EQ(inspector().verifications, 2);
}

TEST(TrustComponent, AnInvalidKnowledgeBasePackOrUnknownFoldersAreProblems) {
  const fakes::FakeClock clock{noon};
  const packs::DiskPackFiles files;
  testing::ComponentHarness harness{clock};
  const std::filesystem::path bad = harness.data_dir() / "packs" / "bad";
  std::filesystem::create_directories(bad);
  std::ofstream(bad / "pack.toml") << "id = \"bad\"\nversion = \"1.0.0\"\ntitle = \"Bad\"\n"
                                      "description = \"An invalid knowledge base.\"\n"
                                      "sources = [\"observed\"]\nmin_myclean = \"0.1.0\"\n";
  std::ofstream(bad / "kb.json") << R"({"entries": [{"id": "x"}]})";
  TrustComponent trust{sources(fail(Errc::io_error, "profiles cannot be listed")), files};
  ASSERT_TRUE(harness.start(trust, harness.data_dir()).has_value());

  EXPECT_FALSE(trust.health().healthy);
  ASSERT_EQ(trust.health().problems.size(), 2U);
  EXPECT_TRUE(trust.health().problems[0].starts_with("knowledge base: "))
      << trust.health().problems[0];
  EXPECT_TRUE(trust.health().problems[1].starts_with("user-writable folders unknown"));
  // Still answers: no entry matches, every path is protected.
  Result<trust::TrustSignal> editor = trust.process(20, noon);
  ASSERT_TRUE(editor.has_value());
  EXPECT_EQ(trust_json(*editor).at("reasons"), (Json{"not-in-knowledge-base", "unsigned"}));
}

}  // namespace
}  // namespace myclean::svc
