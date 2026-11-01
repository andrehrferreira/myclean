#include "core/governor/notify.hpp"

#include <chrono>

#include <gtest/gtest.h>

#include "core/governor/forest.hpp"
#include "fakes/fake_clock.hpp"
#include "ladder_test_support.hpp"

namespace myclean::governor {
namespace {

using std::chrono::minutes;
using std::chrono::seconds;
using testing::key;

class NotificationTest : public ::testing::Test {
 protected:
  fakes::FakeClock clock_{WallTime{}};
  NotificationLimiter limiter_{minutes{5}};
  const ProcessKey tree_ = key(600, 1);
};

TEST_F(NotificationTest, RepeatedInterventionsRateLimited) {
  int shown = 0;
  for (int step = 0; step < 3; ++step) {  // three steps within 2 min
    shown += limiter_.allow(tree_, clock_.monotonic()) ? 1 : 0;
    clock_.advance(seconds{40});
  }
  EXPECT_EQ(shown, 1);
}

TEST_F(NotificationTest, NewNotificationAfter5Min) {
  ASSERT_TRUE(limiter_.allow(tree_, clock_.monotonic()));
  clock_.advance(minutes{5} - seconds{1});
  EXPECT_FALSE(limiter_.allow(tree_, clock_.monotonic()));
  clock_.advance(seconds{1});
  EXPECT_TRUE(limiter_.allow(tree_, clock_.monotonic()));
}

TEST_F(NotificationTest, TreesAreLimitedSeparately) {
  EXPECT_TRUE(limiter_.allow(tree_, clock_.monotonic()));
  EXPECT_TRUE(limiter_.allow(key(700, 2), clock_.monotonic()));
  // The same PID with another creation time is another tree.
  EXPECT_TRUE(limiter_.allow(key(600, 9), clock_.monotonic()));
}

TEST_F(NotificationTest, RecordedNotificationStartsTheInterval) {
  limiter_.record(tree_, clock_.monotonic());
  clock_.advance(minutes{4});
  EXPECT_FALSE(limiter_.allow(tree_, clock_.monotonic()));
  clock_.advance(minutes{1});
  EXPECT_TRUE(limiter_.allow(tree_, clock_.monotonic()));
}

TEST_F(NotificationTest, PruneForgetsOnlyExpiredTrees) {
  ASSERT_TRUE(limiter_.allow(tree_, clock_.monotonic()));
  clock_.advance(minutes{3});
  ASSERT_TRUE(limiter_.allow(key(700, 2), clock_.monotonic()));
  clock_.advance(minutes{2});
  limiter_.prune(clock_.monotonic());
  EXPECT_TRUE(limiter_.allow(tree_, clock_.monotonic()));
  EXPECT_FALSE(limiter_.allow(key(700, 2), clock_.monotonic()));
}

}  // namespace
}  // namespace myclean::governor
