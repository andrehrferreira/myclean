#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cli/text.hpp"

namespace myclean::cli::text {
namespace {

using Json = nlohmann::json;

// Apps spec and backup spec, "Prompt 4/6 Answerable Through MCP": the CLI shows the same data
// as text (and as JSON with --json, the service's answer unchanged).
TEST(GuardText, ShowsTheBackupCheckWithItsSourcesRepositoriesAndAdvice) {
  const Json check{
      {"state", "ready"},
      {"checked_at", 1794355200},
      {"summary", "no backup exists"},
      {"sources", Json::array({Json{{"name", "File History"},
                                    {"status", "not_found"},
                                    {"location", nullptr},
                                    {"newest", nullptr},
                                    {"detail", "File History is not turned on"}},
                               Json{{"name", "Restore points"},
                                    {"status", "could_not_check"},
                                    {"location", nullptr},
                                    {"newest", nullptr},
                                    {"detail", "access denied"}}})},
      {"repositories",
       {{"checked", 3},
        {"unprotected", Json::array({Json{{"path", "F:\\A"}, {"text", "no remote"}}})}}},
      {"recommendation", {{"text", "Turn on File History to K:."}}}};
  const std::string shown = backups(check);
  EXPECT_NE(shown.find("no backup exists"), std::string::npos) << shown;
  EXPECT_NE(shown.find("Restore points: could_not_check (access denied)"), std::string::npos);
  EXPECT_NE(shown.find("F:\\A: no remote"), std::string::npos);
  EXPECT_NE(shown.find("Recommendation: Turn on File History to K:."), std::string::npos);
  EXPECT_NE(backups(Json{{"state", "checking"}}).find("not finished"), std::string::npos);
}

TEST(GuardText, ShowsEachAppWithItsLastUseAndVerdict) {
  const Json list{{"count", 2},
                  {"history", {{"days", 40}}},
                  {"apps", Json::array({Json{{"id", 1},
                                             {"name", "Old Tool"},
                                             {"publisher", "Tools Ltd"},
                                             {"description", "unknown"},
                                             {"last_used", {{"text", "2026-05-10 (Prefetch)"}}},
                                             {"verdict", "unknown"},
                                             {"reason", "not in MyClean's knowledge base"}}})},
                  {"problems", Json::array({"BAM: BAM needs administrator rights"})}};
  const std::string shown = installed_apps(list);
  EXPECT_NE(shown.find("[unknown] Old Tool (Tools Ltd): last used 2026-05-10 (Prefetch)"),
            std::string::npos)
      << shown;
  EXPECT_NE(shown.find("could not read BAM"), std::string::npos);
}

}  // namespace
}  // namespace myclean::cli::text
