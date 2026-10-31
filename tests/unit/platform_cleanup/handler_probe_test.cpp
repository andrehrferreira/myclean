#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "platform/cleanup/win32_handler_host.hpp"

namespace myclean::platform {
namespace {

using std::chrono::seconds;

// The X3 probe against the real machine: Initialize and GetSpaceUsed of every registered
// handler on C:, never Purge, never Deactivate. Passes with or without elevation; the table it
// prints is the evidence of docs/spikes/X3-cleanup-handlers.md.
TEST(HandlerProbe, ScansEveryRegisteredHandlerReadOnly) {
  const Result<std::vector<std::string>> names = Win32HandlerHost::registered_handlers();
  ASSERT_TRUE(names) << names.error().message;
  ASSERT_FALSE(names->empty());

  Win32HandlerHost host{seconds(60), seconds(1)};
  int scanned = 0;
  for (const std::string& name : *names) {
    const HandlerProbe probe = host.probe(name, "C:\\", std::stop_token{});
    const Result<void> status = probe_status(probe);
    std::cout << std::format(
        "{} | {} | ex={} | init=0x{:08X} | flags=0x{:03X} | scan=0x{:08X} | bytes={} | "
        "window={} | timeout={} | {} ms | {}\n",
        name, probe.clsid, probe.initialize_ex, static_cast<std::uint32_t>(probe.initialize_result),
        probe.flags, static_cast<std::uint32_t>(probe.scan_result), probe.space_used.count(),
        probe.showed_window, probe.timed_out, probe.elapsed.count(),
        status ? std::string("ok") : status.error().message);
    EXPECT_FALSE(probe.abandoned) << name;
    if (status) {
      ++scanned;
    }
  }
  EXPECT_GT(scanned, 0);
}

}  // namespace
}  // namespace myclean::platform
