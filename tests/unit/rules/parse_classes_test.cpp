#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {
namespace {

using namespace myclean::literals;

const std::filesystem::path class_file = R"(C:\MyClean\data\classes.toml)";

std::string rejection(std::string_view text) {
  const Result<ClassSet> classes = parse_class_file(text, class_file);
  EXPECT_FALSE(classes.has_value());
  if (classes.has_value()) {
    return {};
  }
  EXPECT_EQ(classes.error().code, Errc::invalid_data);
  return classes.error().message;
}

TEST(ParseClasses, OrderedMatchesWithEveryCriterion) {
  const Result<ClassSet> classes = parse_class_file(R"(
[[match]]
id = "system.myclean"
class = "system"
image = ["MyClean.exe", "MyCleanTray.exe"]
signer = ["MyClean"]

[[match]]
id = "test.node.jest"
class = "test"
image = ["node.exe"]
command_line = '\bjest(\.js)?\b'

[[match]]
id = "game.library"
class = "game"
path_root = ['E:\Games', 'steamapps\common']
)",
                                                    class_file);
  ASSERT_TRUE(classes.has_value()) << classes.error().message;
  ASSERT_EQ(classes->matches.size(), 3U);

  const ClassMatch& system = classes->matches[0];
  EXPECT_EQ(system.id, "system.myclean");
  EXPECT_EQ(system.process_class, ProcessClass::system);
  EXPECT_EQ(system.images, (std::vector<std::string>{"MyClean.exe", "MyCleanTray.exe"}));
  EXPECT_EQ(system.signers, std::vector<std::string>{"MyClean"});
  EXPECT_FALSE(system.command_line.has_value());

  const ClassMatch& jest = classes->matches[1];
  EXPECT_EQ(jest.process_class, ProcessClass::test);
  ASSERT_TRUE(jest.command_line.has_value());
  const Pattern command_line = jest.command_line.value_or(Pattern{});
  EXPECT_EQ(command_line.text, R"(\bjest(\.js)?\b)");
  // Compiled case-insensitive.
  EXPECT_TRUE(
      std::regex_search(R"(node.exe C:\app\node_modules\JEST.js --runInBand)", command_line.regex));
  EXPECT_FALSE(std::regex_search("node.exe vite build", command_line.regex));

  EXPECT_EQ(classes->matches[2].process_class, ProcessClass::game);
  EXPECT_EQ(classes->matches[2].path_roots,
            (std::vector<std::string>{R"(E:\Games)", R"(steamapps\common)"}));
}

TEST(ParseClasses, DefaultCapsFromTheGovernorSpec) {
  const Result<ClassSet> classes = parse_class_file("", class_file);
  ASSERT_TRUE(classes.has_value());
  EXPECT_TRUE(classes->matches.empty());
  EXPECT_EQ(classes->build.cpu_percent, 75);
  EXPECT_EQ(classes->build.commit, 96_GiB);
  EXPECT_EQ(classes->test.cpu_percent, 75);
  EXPECT_EQ(classes->test.commit, 64_GiB);
}

TEST(ParseClasses, CapsOverridePerClass) {
  const Result<ClassSet> classes = parse_class_file(R"(
[caps.build]
cpu_percent = 60
[caps.test]
commit_gib = 32
)",
                                                    class_file);
  ASSERT_TRUE(classes.has_value()) << classes.error().message;
  EXPECT_EQ(classes->build.cpu_percent, 60);
  EXPECT_EQ(classes->build.commit, 96_GiB);
  EXPECT_EQ(classes->test.cpu_percent, 75);
  EXPECT_EQ(classes->test.commit, 32_GiB);
}

// Governor spec, "Invalid class file keeps previous classes": the parser's part.
TEST(ParseClasses, MalformedRegularExpressionRejectsFile) {
  const std::string message = rejection(R"(
[[match]]
id = "build.cl"
class = "build"
image = ["cl.exe"]

[[match]]
id = "test.node.jest"
class = "test"
image = ["node.exe"]
command_line = 'jest(\.js'
)");
  EXPECT_NE(message.find(R"(C:\MyClean\data\classes.toml: match #2 'test.node.jest': field )"
                         R"('command_line' is not a valid regular expression)"),
            std::string::npos)
      << message;
}

TEST(ParseClasses, UnknownClassRejected) {
  EXPECT_NE(rejection("[[match]]\nid = \"x\"\nclass = \"ide\"\nimage = [\"code.exe\"]\n")
                .find("match #1 'x': field 'class' must be one of system, game, allow, build, "
                      "test or default, not 'ide'"),
            std::string::npos);
}

TEST(ParseClasses, EntryWithoutCriterionRejected) {
  const std::string message = rejection("[[match]]\nid = \"everything\"\nclass = \"allow\"\n");
  EXPECT_NE(
      message.find("match #1 'everything': sets none of image, command_line, path_root and signer"),
      std::string::npos)
      << message;
}

TEST(ParseClasses, ImageWithFolderRejected) {
  EXPECT_NE(rejection("[[match]]\nid = \"x\"\nclass = \"build\"\nimage = ['C:\\bin\\cl.exe']\n")
                .find("field 'image' must hold image names without a folder"),
            std::string::npos);
}

TEST(ParseClasses, MisspelledKeysAndDuplicateIdsRejected) {
  EXPECT_NE(rejection("[[match]]\nid = \"x\"\nclass = \"build\"\nimages = [\"cl.exe\"]\n")
                .find("match #1 'x': field 'images' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection("[[class]]\nid = \"x\"\n").find("field 'class' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection("[[match]]\nid = \"x\"\nclass = \"build\"\nimage = [\"cl.exe\"]\n"
                      "[[match]]\nid = \"x\"\nclass = \"test\"\nimage = [\"pytest.exe\"]\n")
                .find("match #2 'x': field 'id' repeats an id"),
            std::string::npos);
}

TEST(ParseClasses, InvalidCapsRejected) {
  EXPECT_NE(rejection("[caps.game]\ncpu_percent = 50\n")
                .find("classes.toml: field 'caps.game' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection("[caps.build]\ncpu_percent = 0\n")
                .find("field 'caps.build.cpu_percent' must be between 1 and 100"),
            std::string::npos);
  EXPECT_NE(rejection("[caps.test]\ncommit_gb = 64\n")
                .find("field 'caps.test.commit_gb' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection("[caps.build]\ncommit_gib = 9.5\n")
                .find("field 'caps.build.commit_gib' must be an integer"),
            std::string::npos);
}

TEST(ProcessClass, TextFormsRoundTrip) {
  for (const ProcessClass process_class :
       {ProcessClass::system, ProcessClass::game, ProcessClass::allow, ProcessClass::build,
        ProcessClass::test, ProcessClass::normal}) {
    EXPECT_EQ(parse_process_class(to_string(process_class)), process_class);
  }
  EXPECT_EQ(to_string(ProcessClass::normal), "default");
}

}  // namespace
}  // namespace myclean::rules
