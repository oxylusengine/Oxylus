#include <gtest/gtest.h>

#include "Utils/PendingPick.hpp"

namespace ox {
struct PickCompletion {
  vir::SubmitTicket completed = {};

  auto is_retired(this const PickCompletion& self, vir::SubmitTicket ticket) -> bool {
    return ticket.queue == self.completed.queue && ticket.value <= self.completed.value;
  }
};

TEST(PendingPick, WaitsForTheSubmissionThatWritesTheResult) {
  auto pick = PendingPick{};
  auto completion = PickCompletion{.completed = {vir::QueueID{0}, 10}};
  ASSERT_TRUE(pick.request());
  EXPECT_FALSE(pick.is_ready(completion));
  pick.submitted(vir::SubmitTicket{vir::QueueID{0}, 11});
  EXPECT_FALSE(pick.is_ready(completion));
  completion.completed = {vir::QueueID{1}, 20};
  EXPECT_FALSE(pick.is_ready(completion));
  completion.completed = {vir::QueueID{0}, 11};
  EXPECT_TRUE(pick.is_ready(completion));
}

TEST(PendingPick, DoesNotReuseAnOutstandingReadbackOrReplaceItsTicket) {
  auto pick = PendingPick{};
  ASSERT_TRUE(pick.request());
  pick.submitted(vir::SubmitTicket{vir::QueueID{0}, 5});
  EXPECT_FALSE(pick.request());
  pick.submitted(vir::SubmitTicket{vir::QueueID{0}, 6});
  EXPECT_EQ(pick.ticket.value, 5);
  pick.submitted(std::unexpected(VK_ERROR_DEVICE_LOST));
  EXPECT_TRUE(pick.pending);
  EXPECT_EQ(pick.ticket.value, 5);
}

TEST(PendingPick, FailedOrEmptySubmissionsCancelOnlyUnsubmittedRequests) {
  auto pick = PendingPick{};
  auto completion = PickCompletion{.completed = {vir::QueueID{0}, 10}};
  ASSERT_TRUE(pick.request());
  pick.submitted(std::unexpected(VK_ERROR_INITIALIZATION_FAILED));
  EXPECT_FALSE(pick.pending);
  EXPECT_FALSE(pick.is_ready(completion));
  ASSERT_TRUE(pick.request());
  pick.submitted(vir::SubmitTicket{});
  EXPECT_FALSE(pick.pending);
  EXPECT_FALSE(pick.is_ready(completion));
  ASSERT_TRUE(pick.request());
  pick.submitted(vir::SubmitTicket{vir::QueueID{0}, 8});
  EXPECT_TRUE(pick.is_ready(completion));
}
} // namespace ox
