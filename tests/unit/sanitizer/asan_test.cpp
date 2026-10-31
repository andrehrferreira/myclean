#include <cstddef>
#include <memory>

#include <gtest/gtest.h>

namespace myclean {
namespace {

// Reads one element past the end of a heap buffer.
int read_past_end(std::size_t size) {
  const auto buffer = std::make_unique<int[]>(size);  // NOLINT(*-avoid-c-arrays): heap block
  const int* const data = buffer.get();
  return data[size];  // NOLINT(*-pro-bounds-pointer-arithmetic): the deliberate overflow
}

// Proves that the debug preset really builds with AddressSanitizer: the overflow must abort
// the death-test child with an ASan report.
TEST(AddressSanitizer, ReportsHeapBufferOverflow) {
  EXPECT_DEATH(static_cast<void>(read_past_end(4)), "heap-buffer-overflow");
}

}  // namespace
}  // namespace myclean
