#include "core/governor/classifier.hpp"

#include <regex>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {
namespace {

using rules::ProcessClass;

rules::Pattern pattern(const std::string& text) {
  return {.text = text, .regex = std::regex(text, std::regex::ECMAScript | std::regex::icase)};
}

rules::ClassMatch images(std::string id, ProcessClass process_class,
                         std::vector<std::string> names) {
  return {.id = std::move(id), .process_class = process_class, .images = std::move(names)};
}

ProcessFacts process(const std::string& image, std::string command_line = {}) {
  return {.image = image,
          .image_path = "C:\\Tools\\" + image,
          .command_line = std::move(command_line),
          .signer = std::nullopt};
}

// ---- spec scenarios -----------------------------------------------------------------------

TEST(Classification, BuildToolClassifiedByImage) {
  const rules::ClassSet classes{
      .matches = {images("build.native", ProcessClass::build, {"cl.exe", "rustc.exe"})}};
  const Classification result = classify(process("cl.exe"), classes);
  EXPECT_EQ(result.process_class, ProcessClass::build);
  EXPECT_EQ(result.match_id, "build.native");
}

TEST(Classification, CommandLineDecidesTheClass) {
  rules::ClassMatch test{.id = "test.node.jest",
                         .process_class = ProcessClass::test,
                         .images = {"node.exe"},
                         .command_line = pattern(R"(\bjest(\.js)?\b)")};
  rules::ClassMatch build{.id = "build.node.vite",
                          .process_class = ProcessClass::build,
                          .images = {"node.exe"},
                          .command_line = pattern(R"(\bvite(\.js)?\b)")};
  const rules::ClassSet classes{.matches = {std::move(test), std::move(build)}};
  EXPECT_EQ(
      classify(process("node.exe", R"(node.exe C:\app\node_modules\jest\bin\jest.js --runInBand)"),
               classes)
          .process_class,
      ProcessClass::test);
  EXPECT_EQ(classify(process("node.exe", R"(node.exe C:\app\node_modules\vite\bin\vite.js build)"),
                     classes)
                .process_class,
            ProcessClass::build);
  EXPECT_EQ(classify(process("node.exe", "node.exe server.js"), classes).process_class,
            ProcessClass::normal);
}

TEST(Classification, FirstMatchWins) {
  const rules::ClassSet classes{
      .matches = {images("myclean", ProcessClass::system, {"myclean-svc.exe"}),
                  images("build.all-exe", ProcessClass::build, {"*.exe"})}};
  const Classification result = classify(process("myclean-svc.exe"), classes);
  EXPECT_EQ(result.process_class, ProcessClass::system);
  EXPECT_EQ(result.match_id, "myclean");
  EXPECT_EQ(classify(process("cargo.exe"), classes).process_class, ProcessClass::build);
}

TEST(Classification, InvalidClassFileKeepsPreviousClasses) {
  Classifier classifier(
      rules::ClassSet{.matches = {images("build.native", ProcessClass::build, {"cl.exe"})}});
  const Result<void> reloaded = classifier.reload(rules::parse_class_file(
      "[[match]]\nid = \"broken\"\nclass = \"build\"\nimage = [\"node.exe\"]\n"
      "command_line = \"(unclosed\"\n",
      "classes.toml"));
  ASSERT_FALSE(reloaded.has_value());
  EXPECT_EQ(reloaded.error().code, Errc::invalid_data);
  EXPECT_NE(reloaded.error().message.find("command_line"), std::string::npos);
  ASSERT_EQ(classifier.classes().matches.size(), 1U);
  EXPECT_EQ(classifier.classify(process("cl.exe")).process_class, ProcessClass::build);
}

TEST(Classification, ValidReloadReplacesTheClasses) {
  Classifier classifier(
      rules::ClassSet{.matches = {images("build.native", ProcessClass::build, {"cl.exe"})}});
  ASSERT_TRUE(classifier
                  .reload(rules::parse_class_file(
                      "[[match]]\nid = \"t\"\nclass = \"test\"\nimage = [\"cl.exe\"]\n", "c.toml"))
                  .has_value());
  EXPECT_EQ(classifier.classify(process("cl.exe")).process_class, ProcessClass::test);
}

// ---- matching rules -----------------------------------------------------------------------

TEST(Classification, UnmatchedProcessIsDefault) {
  const Classification result = classify(process("notepad.exe"), rules::ClassSet{});
  EXPECT_EQ(result.process_class, ProcessClass::normal);
  EXPECT_TRUE(result.match_id.empty());
}

TEST(ImageMatching, IgnoresCaseAndHonoursWildcards) {
  EXPECT_TRUE(image_matches("CL.EXE", "cl.exe"));
  EXPECT_TRUE(image_matches("*_tests.exe", "myclean_governor_tests.exe"));
  EXPECT_TRUE(image_matches("testhost*.exe", "testhost.x86.exe"));
  EXPECT_TRUE(image_matches("rust?.exe", "rustc.exe"));
  EXPECT_TRUE(image_matches("*", "anything.exe"));
  EXPECT_TRUE(image_matches("a*b*c", "aXXbYYbZc"));
  EXPECT_FALSE(image_matches("rust?.exe", "rust.exe"));
  EXPECT_FALSE(image_matches("cl.exe", "cl.exe.bak"));
  EXPECT_FALSE(image_matches("*_tests.exe", "tests.exe"));
  EXPECT_FALSE(image_matches("", "cl.exe"));
}

TEST(PathRoots, PrefixAtAFolderBoundaryIgnoringCase) {
  EXPECT_TRUE(under_path_root(R"(E:\Jogos)", R"(e:\jogos\Title\title.exe)"));
  EXPECT_TRUE(under_path_root(R"(E:\Jogos\)", R"(E:\Jogos\title.exe)"));
  EXPECT_TRUE(under_path_root("E:/Jogos", R"(E:\Jogos\title.exe)"));
  EXPECT_TRUE(under_path_root(R"(E:\)", R"(E:\title.exe)"));
  EXPECT_FALSE(under_path_root(R"(E:\Jogos)", R"(E:\JogosOld\title.exe)"));
  EXPECT_FALSE(under_path_root(R"(E:\Jogos)", R"(D:\Jogos\title.exe)"));
  EXPECT_FALSE(under_path_root("", R"(E:\title.exe)"));
}

TEST(Classification, EveryCriterionOfAnEntryMustMatch) {
  const rules::ClassSet classes{.matches = {{.id = "game.library",
                                             .process_class = ProcessClass::game,
                                             .images = {"*.exe"},
                                             .path_roots = {R"(E:\Jogos)"},
                                             .signers = {"Valve Corp."}}}};
  ProcessFacts facts{
      .image = "title.exe", .image_path = R"(E:\Jogos\Title\title.exe)", .signer = "VALVE CORP."};
  EXPECT_EQ(classify(facts, classes).process_class, ProcessClass::game);
  facts.signer.reset();  // no signer: the signer criterion cannot match
  EXPECT_EQ(classify(facts, classes).process_class, ProcessClass::normal);
  facts.signer = "Valve Corp.";
  facts.image_path = R"(C:\Other\title.exe)";
  EXPECT_EQ(classify(facts, classes).process_class, ProcessClass::normal);
}

TEST(Classification, UnreadableCommandLineDoesNotMatchAPattern) {
  const rules::ClassSet classes{.matches = {{.id = "any",
                                             .process_class = ProcessClass::build,
                                             .images = {"node.exe"},
                                             .command_line = pattern(".*")}}};
  EXPECT_EQ(classify(process("node.exe"), classes).process_class, ProcessClass::normal);
}

TEST(FinalClass, DetectedGameIsGameUnlessSystem) {
  EXPECT_EQ(final_class(ProcessClass::normal, true), ProcessClass::game);
  EXPECT_EQ(final_class(ProcessClass::build, true), ProcessClass::game);
  EXPECT_EQ(final_class(ProcessClass::system, true), ProcessClass::system);
  EXPECT_EQ(final_class(ProcessClass::build, false), ProcessClass::build);
}

}  // namespace
}  // namespace myclean::governor
