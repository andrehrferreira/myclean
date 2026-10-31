#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/cli.hpp"
#include "core/os/clock.hpp"

namespace myclean::cli {
namespace {

struct Outcome {
  int code = 0;
  std::string out;
  std::string err;
};

Outcome run_scan_all(std::vector<std::string> args) {
  args.insert(args.begin(), "scan-all");
  std::istringstream in;
  std::ostringstream out;
  std::ostringstream err;
  const os::SystemClock clock;
  const int code = run(args, in, out, err, clock);
  return {.code = code, .out = out.str(), .err = err.str()};
}

TEST(ScanAllCli, RefusesBadArgumentsWithItsUsage) {
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"--budget-minutes", "0"},
        std::vector<std::string>{"--budget-minutes", "ten"},
        std::vector<std::string>{"--drives", "1:"}, std::vector<std::string>{"--drives", "C:,DE"},
        std::vector<std::string>{"--bogus", "x"}, std::vector<std::string>{"--drives"}}) {
    const Outcome outcome = run_scan_all(args);
    EXPECT_EQ(outcome.code, exit_usage) << args.front();
    EXPECT_TRUE(outcome.out.empty());
    EXPECT_NE(outcome.err.find("usage: myclean scan-all"), std::string::npos) << outcome.err;
  }
}

}  // namespace
}  // namespace myclean::cli
