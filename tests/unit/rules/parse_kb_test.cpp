#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {
namespace {

const std::filesystem::path kb_file = R"(C:\MyClean\data\kb\processes.json)";

// One knowledge-base file holding a single entry whose members are `members`.
std::string kb_with(std::string_view members) {
  return R"({ "entries": [ { )" + std::string(members) + " } ] }";
}

constexpr std::string_view complete_entry =
    R"("id": "powertoys.kbm", "image": "PowerToys.KeyboardManagerEngine.exe",
       "description": "Remaps keys.", "category": "utility",
       "needed_at_startup": false, "safe_to_remove": true)";

std::string rejection(std::string_view text) {
  const Result<std::vector<KbEntry>> entries = parse_kb_file(text, kb_file);
  EXPECT_FALSE(entries.has_value());
  if (entries.has_value()) {
    return {};
  }
  EXPECT_EQ(entries.error().code, Errc::invalid_data);
  return entries.error().message;
}

TEST(ParseKb, CompleteEntry) {
  const Result<std::vector<KbEntry>> entries = parse_kb_file(R"({ "entries": [
    { "id": "microsoft.powertoys.keyboard-manager",
      "image": "PowerToys.KeyboardManagerEngine.exe",
      "signer": "Microsoft Corporation",
      "path": "\\\\PowerToys\\\\",
      "description": "Part of PowerToys: remaps keys and shortcuts.",
      "category": "utility",
      "needed_at_startup": false,
      "safe_to_remove": true },
    { "id": "windows.svchost", "image": "svchost.exe", "description": "Hosts Windows services.",
      "category": "system", "needed_at_startup": true, "safe_to_remove": false } ] })",
                                                             kb_file);
  ASSERT_TRUE(entries.has_value()) << entries.error().message;
  ASSERT_EQ(entries->size(), 2U);
  const KbEntry& powertoys = (*entries)[0];
  EXPECT_EQ(powertoys.id, "microsoft.powertoys.keyboard-manager");
  EXPECT_EQ(powertoys.image, "PowerToys.KeyboardManagerEngine.exe");
  EXPECT_EQ(powertoys.signer, "Microsoft Corporation");
  ASSERT_TRUE(powertoys.path.has_value());
  const Pattern path = powertoys.path.value_or(Pattern{});
  EXPECT_EQ(path.text, R"(\\PowerToys\\)");
  EXPECT_TRUE(
      std::regex_search(R"(C:\Program Files\powertoys\KeyboardManagerEngine\x.exe)", path.regex));
  EXPECT_EQ(powertoys.category, "utility");
  EXPECT_FALSE(powertoys.needed_at_startup);
  EXPECT_TRUE(powertoys.safe_to_remove);
  EXPECT_FALSE((*entries)[1].signer.has_value());
  EXPECT_FALSE((*entries)[1].path.has_value());
  EXPECT_TRUE((*entries)[1].needed_at_startup);
}

// Trust spec, "Invalid entry".
TEST(ParseKb, EntryWithoutDescriptionRejectsFileNamingFileAndEntry) {
  const std::string text = R"({ "entries": [ { )" + std::string(complete_entry) + R"( },
    { "id": "anydesk", "image": "AnyDesk.exe", "category": "remote access",
      "needed_at_startup": false, "safe_to_remove": true } ] })";
  EXPECT_EQ(rejection(text),
            R"(C:\MyClean\data\kb\processes.json: entry #2 'anydesk': field 'description' )"
            R"(is missing)");
}

TEST(ParseKb, SafetyFlagsAreRequiredBooleans) {
  EXPECT_NE(rejection(kb_with(R"("id": "x", "image": "x.exe", "description": "d",
                                 "category": "c", "needed_at_startup": false)"))
                .find("entry #1 'x': field 'safe_to_remove' is missing"),
            std::string::npos);
  EXPECT_NE(rejection(kb_with(R"("id": "x", "image": "x.exe", "description": "d",
                                 "category": "c", "needed_at_startup": "no",
                                 "safe_to_remove": false)"))
                .find("field 'needed_at_startup' must be true or false"),
            std::string::npos);
}

TEST(ParseKb, MisspelledKeysRejected) {
  EXPECT_NE(rejection(kb_with(std::string(complete_entry) + R"(, "sigher": "Microsoft")"))
                .find("entry #1 'powertoys.kbm': field 'sigher' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection(R"({ "entries": [], "version": 2 })")
                .find("processes.json: field 'version' is not a known field"),
            std::string::npos);
}

TEST(ParseKb, DuplicateIdRejected) {
  const std::string entry = "{ " + std::string(complete_entry) + " }";
  EXPECT_NE(rejection(R"({ "entries": [ )" + entry + ", " + entry + " ] }")
                .find("entry #2 'powertoys.kbm': field 'id' repeats an id"),
            std::string::npos);
}

TEST(ParseKb, MalformedPathPatternRejected) {
  EXPECT_NE(rejection(kb_with(std::string(complete_entry) + R"(, "path": "[unclosed")"))
                .find("field 'path' is not a valid regular expression"),
            std::string::npos);
}

TEST(ParseKb, WrongDocumentShapesRejected) {
  EXPECT_NE(rejection(R"({ "entries": [ )").find("processes.json: is not valid JSON"),
            std::string::npos);
  EXPECT_NE(rejection("[]").find("must be a JSON object with an \"entries\" array"),
            std::string::npos);
  EXPECT_NE(rejection("{}").find("field 'entries' must be an array"), std::string::npos);
  EXPECT_NE(rejection(R"({ "entries": [ 3 ] })").find("entry #1: must be a JSON object"),
            std::string::npos);
  EXPECT_NE(rejection(kb_with(R"("id": "x", "image": "")"))
                .find("entry #1 'x': field 'image' must be a non-empty string"),
            std::string::npos);
}

TEST(ParseKb, EmptyEntriesIsValid) {
  const Result<std::vector<KbEntry>> entries = parse_kb_file(R"({ "entries": [] })", kb_file);
  ASSERT_TRUE(entries.has_value());
  EXPECT_TRUE(entries->empty());
}

}  // namespace
}  // namespace myclean::rules
