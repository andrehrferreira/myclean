#include "core/trust/trust.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"

namespace myclean::trust {
namespace {

using std::filesystem::path;

rules::KbEntry entry(std::string id, std::string image, std::optional<std::string> signer,
                     std::optional<std::string> path_pattern = std::nullopt) {
  rules::KbEntry result{.id = std::move(id),
                        .image = std::move(image),
                        .signer = std::move(signer),
                        .description = "Plain description.",
                        .category = "utility"};
  if (path_pattern) {
    result.path = rules::Pattern{
        .text = *path_pattern,
        .regex = std::regex(*path_pattern, std::regex::ECMAScript | std::regex::icase)};
  }
  return result;
}

const std::vector<rules::KbEntry>& knowledge_base() {
  static const std::vector<rules::KbEntry> kb = {
      entry("microsoft.powertoys.keyboard-manager", "PowerToys.KeyboardManagerEngine.exe",
            "Microsoft Corporation", R"(\\PowerToys\\)"),
      entry("example.tool", "tool.exe", std::nullopt),
      entry("contoso.app", "contoso.exe", "Contoso Ltd"),
  };
  return kb;
}

const std::vector<path> roots = {L"C:\\Users\\owner", L"C:\\Windows\\Temp"};

const Signature unsigned_image{.status = SignatureStatus::none, .signer = std::nullopt};
const Signature by_contoso{.status = SignatureStatus::valid, .signer = "Contoso Ltd"};

TEST(PathClass, ImageInAppDataIsUserWritable) {
  EXPECT_EQ(classify_path(L"C:\\Users\\owner\\AppData\\Local\\Foo\\foo.exe", roots),
            PathClass::user_writable);
}

TEST(PathClass, ImageInProgramFilesIsProtected) {
  EXPECT_EQ(classify_path(L"C:\\Program Files\\Foo\\foo.exe", roots),
            PathClass::protected_location);
}

TEST(PathClass, ComparisonIgnoresCaseAndSeparatorsButNotNameBoundaries) {
  EXPECT_EQ(classify_path(L"c:/USERS/Owner/Downloads/setup.exe", roots), PathClass::user_writable);
  EXPECT_EQ(classify_path(L"C:\\Windows\\Temp\\x.exe", roots), PathClass::user_writable);
  EXPECT_EQ(classify_path(L"C:\\Users\\owner2\\x.exe", roots), PathClass::protected_location);
  EXPECT_EQ(classify_path(L"C:\\Windows\\System32\\x.exe", roots), PathClass::protected_location);
  EXPECT_EQ(classify_path(L"C:\\Users\\owner\\x.exe", std::vector<path>{L"C:\\Users\\owner\\"}),
            PathClass::user_writable);
}

TEST(KbMatch, MatchesNameIgnoringCaseSignerAndPathPattern) {
  const path image =
      L"C:\\Program Files\\PowerToys\\modules\\KeyboardManager\\KeyboardManagerEngine\\"
      L"powertoys.keyboardmanagerengine.EXE";
  const rules::KbEntry* found = match_entry(knowledge_base(), image, "microsoft corporation");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, "microsoft.powertoys.keyboard-manager");
}

TEST(KbMatch, LookAlikeWithoutTheSignerOrOutsideThePathDoesNotMatch) {
  const path elsewhere =
      L"C:\\Users\\owner\\AppData\\Local\\Temp\\PowerToys.KeyboardManagerEngine.exe";
  const path installed = L"C:\\Program Files\\PowerToys\\PowerToys.KeyboardManagerEngine.exe";
  EXPECT_EQ(match_entry(knowledge_base(), installed, std::nullopt), nullptr);
  EXPECT_EQ(match_entry(knowledge_base(), installed, "Someone Else"), nullptr);
  EXPECT_EQ(match_entry(knowledge_base(), elsewhere, "Microsoft Corporation"), nullptr);
  EXPECT_NE(match_entry(knowledge_base(), installed, "Microsoft Corporation"), nullptr);
}

TEST(TrustSignal, UnknownUnsignedProcessIsUnrecognized) {
  const TrustSignal signal = assess(L"C:\\Program Files\\Odd\\odd.exe", unsigned_image,
                                    PathClass::protected_location, knowledge_base());
  EXPECT_TRUE(signal.unrecognized());
  EXPECT_EQ(signal.reasons,
            (std::vector<Reason>{Reason::not_in_knowledge_base, Reason::unsigned_image}));
}

TEST(TrustSignal, UnknownProcessFromUserWritablePathIsUnrecognized) {
  const TrustSignal signal = assess(L"C:\\Users\\owner\\AppData\\Local\\Temp\\other.exe",
                                    by_contoso, PathClass::user_writable, knowledge_base());
  EXPECT_EQ(signal.reasons,
            (std::vector<Reason>{Reason::not_in_knowledge_base, Reason::user_writable_path}));
}

TEST(TrustSignal, KnownProcessThatIsUnsignedIsNotUnrecognized) {
  const TrustSignal signal = assess(L"C:\\Users\\owner\\tool.exe", unsigned_image,
                                    PathClass::user_writable, knowledge_base());
  EXPECT_FALSE(signal.unrecognized());
  EXPECT_EQ(signal.entry.value_or(rules::KbEntry{}).id, "example.tool");
}

TEST(TrustSignal, UnknownButSignedByListedSignerInProgramFilesIsNotUnrecognized) {
  const TrustSignal signal = assess(L"C:\\Program Files\\Contoso\\other.exe", by_contoso,
                                    PathClass::protected_location, knowledge_base());
  EXPECT_FALSE(signal.unrecognized());
  EXPECT_FALSE(signal.entry.has_value());
}

TEST(TrustSignal, InvalidSignatureAndUnlistedSignerAreReasons) {
  const TrustSignal invalid =
      assess(L"C:\\Program Files\\X\\x.exe", Signature{.status = SignatureStatus::invalid},
             PathClass::protected_location, knowledge_base());
  EXPECT_EQ(invalid.reasons,
            (std::vector<Reason>{Reason::not_in_knowledge_base, Reason::invalid_signature}));
  const TrustSignal unlisted = assess(
      L"C:\\Users\\owner\\x.exe", Signature{.status = SignatureStatus::valid, .signer = "Nobody"},
      PathClass::user_writable, knowledge_base());
  EXPECT_EQ(unlisted.reasons,
            (std::vector<Reason>{Reason::not_in_knowledge_base, Reason::unlisted_signer,
                                 Reason::user_writable_path}));
}

TEST(TrustSignal, TextFormsReportFactsOnly) {
  EXPECT_EQ(to_string(SignatureStatus::valid), "signed");
  EXPECT_EQ(to_string(SignatureStatus::none), "unsigned");
  EXPECT_EQ(to_string(SignatureStatus::invalid), "invalid");
  EXPECT_EQ(to_string(PathClass::protected_location), "protected");
  EXPECT_EQ(to_string(PathClass::user_writable), "user-writable");
  EXPECT_EQ(to_string(Reason::not_in_knowledge_base), "not-in-knowledge-base");
  EXPECT_EQ(to_string(Reason::unsigned_image), "unsigned");
  EXPECT_EQ(to_string(Reason::invalid_signature), "invalid-signature");
  EXPECT_EQ(to_string(Reason::unlisted_signer), "signer-not-listed");
  EXPECT_EQ(to_string(Reason::user_writable_path), "user-writable-path");
}

// Files with a stamp and a signature; counts the verifications.
class FakeInspector final : public IImageInspector {
 public:
  void put(const path& image, FileStamp stamp, Signature signature) {
    files_[image] = {stamp, std::move(signature)};
  }

  [[nodiscard]] Result<FileStamp> stamp(const path& image) const override {
    const auto file = files_.find(image);
    if (file == files_.end()) {
      return fail(Errc::not_found, "no such file");
    }
    return file->second.first;
  }

  [[nodiscard]] Result<Signature> verify(const path& image) const override {
    ++verifications_;
    const auto file = files_.find(image);
    if (file == files_.end()) {
      return fail(Errc::not_found, "no such file");
    }
    return file->second.second;
  }

  [[nodiscard]] int verifications() const { return verifications_; }

 private:
  std::map<path, std::pair<FileStamp, Signature>> files_;
  mutable int verifications_ = 0;
};

const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

TEST(TrustEvaluator, SignalHasEveryField) {
  FakeInspector inspector;
  const path image = L"C:\\Users\\owner\\AppData\\Local\\Contoso\\contoso.exe";
  inspector.put(image, FileStamp{.size = 10, .modified = noon}, by_contoso);
  TrustEvaluator evaluator(inspector, roots);
  const Result<TrustSignal> signal = evaluator.evaluate(image, knowledge_base());
  ASSERT_TRUE(signal.has_value()) << signal.error().message;
  EXPECT_EQ(signal->image, image);
  EXPECT_EQ(signal->signature, by_contoso);
  EXPECT_EQ(signal->path_class, PathClass::user_writable);
  EXPECT_EQ(signal->entry.value_or(rules::KbEntry{}).id, "contoso.app");
  EXPECT_FALSE(signal->unrecognized());
}

TEST(TrustEvaluator, SignatureIsCachedUntilTheFileChanges) {
  FakeInspector inspector;
  const path image = L"C:\\Program Files\\Contoso\\contoso.exe";
  inspector.put(image, FileStamp{.size = 10, .modified = noon}, by_contoso);
  TrustEvaluator evaluator(inspector, roots);
  ASSERT_TRUE(evaluator.evaluate(image, knowledge_base()).has_value());
  ASSERT_TRUE(evaluator.evaluate(image, knowledge_base()).has_value());
  EXPECT_EQ(inspector.verifications(), 1);

  inspector.put(image, FileStamp{.size = 11, .modified = noon}, unsigned_image);
  const Result<TrustSignal> changed = evaluator.evaluate(image, knowledge_base());
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(inspector.verifications(), 2);
  EXPECT_EQ(changed->signature, unsigned_image);
  EXPECT_FALSE(changed->entry.has_value());  // the KB entry requires the signer

  inspector.put(image, FileStamp{.size = 11, .modified = noon + std::chrono::seconds{1}},
                unsigned_image);
  ASSERT_TRUE(evaluator.evaluate(image, knowledge_base()).has_value());
  EXPECT_EQ(inspector.verifications(), 3);
}

TEST(TrustEvaluator, UnreadableImageIsAnError) {
  const FakeInspector inspector;
  TrustEvaluator evaluator(inspector, roots);
  const Result<TrustSignal> signal = evaluator.evaluate(L"C:\\gone.exe", knowledge_base());
  ASSERT_FALSE(signal.has_value());
  EXPECT_EQ(signal.error().code, Errc::not_found);
}

}  // namespace
}  // namespace myclean::trust
