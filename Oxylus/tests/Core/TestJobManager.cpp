#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/JobManager.hpp"

class JobManagerTest : public ::testing::Test {
protected:
  // every test starts the pool itself, so it can pick the worker count its expectations need
  auto start_with_threads(u32 thread_count) -> void {
    manager->set_thread_count(thread_count);
    auto init_result = manager->init();
    ASSERT_TRUE(init_result.has_value());
    ASSERT_EQ(manager->get_thread_count(), thread_count);
  }

  void SetUp() override { manager = std::make_unique<ox::JobManager>(); }

  void TearDown() override {
    auto deinit_result = manager->deinit();
    EXPECT_TRUE(deinit_result.has_value());
  }

  std::unique_ptr<ox::JobManager> manager = nullptr;
};

// --- Basic Functionality Tests ---

TEST_F(JobManagerTest, ExecutesSingleJob) {
  start_with_threads(4);

  std::atomic<bool> executed{false};

  manager->push_job_name("TestJob");
  auto job = ox::Job::create([&] { executed = true; });
  manager->pop_job_name();

  manager->submit(job);
  manager->wait();

  EXPECT_TRUE(executed);
}

TEST_F(JobManagerTest, ExecutesMultipleJobsInOrder) {
  // the queue is FIFO but only one worker can make the execution order match the submit order
  start_with_threads(1);

  std::vector<int> execution_order;
  std::mutex order_mutex;

  manager->push_job_name("OrderTest");

  for (int i = 0; i < 5; ++i) {
    auto job = ox::Job::create([&, i] {
      std::lock_guard lock(order_mutex);
      execution_order.push_back(i);
    });
    manager->submit(job);
  }
  manager->pop_job_name();

  manager->wait();
  EXPECT_EQ(execution_order, std::vector<int>({0, 1, 2, 3, 4}));
}

// --- Thread Safety Tests ---

TEST_F(JobManagerTest, HandlesConcurrentAccess) {
  start_with_threads(4);

  std::atomic<int> counter{0};
  constexpr int kIterations = 1000;

  manager->push_job_name("ConcurrentTest");
  for (int i = 0; i < kIterations; ++i) {
    manager->submit(ox::Job::create([&] { counter.fetch_add(1, std::memory_order_relaxed); }));
  }
  manager->pop_job_name();

  manager->wait();
  EXPECT_EQ(counter.load(), kIterations);
}

TEST_F(JobManagerTest, SafeWithSimultaneousSubmitAndShutdown) {
  start_with_threads(4);

  manager->push_job_name("StressTest");
  std::thread worker([this] {
    for (int i = 0; i < 100; ++i) {
      manager->submit(ox::Job::create([] {}));
    }
  });
  manager->pop_job_name();

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  manager->shutdown();
  worker.join();

  SUCCEED(); // Just testing we don't crash
}

// --- Tracking System Tests ---

// spins until `predicate` holds, workers pick jobs up on their own schedule
template <typename Predicate>
static auto eventually(Predicate&& predicate) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }

    std::this_thread::yield();
  }

  return predicate();
}

static auto find_record(std::span<const ox::JobRecord> records, ox::JobID id) -> const ox::JobRecord* {
  auto it = std::ranges::find(records, id, &ox::JobRecord::id);
  return it == records.end() ? nullptr : &*it;
}

struct Gate {
  std::atomic<bool> is_open{false};

  auto wait(this Gate& self) -> void { self.is_open.wait(false, std::memory_order_acquire); }
  auto open(this Gate& self) -> void {
    self.is_open.store(true, std::memory_order_release);
    self.is_open.notify_all();
  }
};

TEST_F(JobManagerTest, TracksJobLifecycle) {
  start_with_threads(4);
  manager->get_tracker().start_tracking();

  Gate gate;
  manager->push_job_name("TrackedJob");
  auto job = ox::Job::create([&] { gate.wait(); });
  manager->submit(job);
  manager->pop_job_name();

  auto snapshot = ox::JobSnapshot{};
  ASSERT_TRUE(eventually([&] {
    manager->get_tracker().snapshot(snapshot);
    return snapshot.live.size() == 1 && snapshot.live[0].state == ox::JobState::Running;
  }));

  const auto& running = snapshot.live[0];
  EXPECT_EQ(running.id, job->id);
  EXPECT_EQ(running.name, "TrackedJob");
  EXPECT_LT(running.worker, 4u);
  EXPECT_EQ(running.parent, ox::JobID::Invalid);
  EXPECT_THAT(running.location.file_name(), ::testing::HasSubstr("TestJobManager.cpp"));

  auto names = std::vector<std::string>();
  manager->get_tracker().active_names(names);
  EXPECT_THAT(names, ::testing::ElementsAre("TrackedJob"));

  gate.open();
  manager->wait();

  manager->get_tracker().snapshot(snapshot);
  EXPECT_TRUE(snapshot.live.empty());
  ASSERT_EQ(snapshot.history.size(), 1);
  EXPECT_EQ(snapshot.history[0].state, ox::JobState::Done);
  EXPECT_GE(snapshot.history[0].finished_at, snapshot.history[0].started_at);
  ASSERT_EQ(snapshot.stats.size(), 1);
  EXPECT_EQ(snapshot.stats[0].label, "TrackedJob");
  EXPECT_EQ(snapshot.stats[0].completed, 1u);

  manager->get_tracker().active_names(names);
  EXPECT_TRUE(names.empty());
}

TEST_F(JobManagerTest, NoTrackingWhenDisabled) {
  start_with_threads(4);

  manager->get_tracker().stop_tracking();

  manager->push_job_name("Untracked");
  manager->submit(ox::Job::create([] {}));
  manager->pop_job_name();
  manager->wait();

  auto snapshot = ox::JobSnapshot{};
  manager->get_tracker().snapshot(snapshot);
  EXPECT_TRUE(snapshot.live.empty());
  EXPECT_TRUE(snapshot.history.empty());
  EXPECT_TRUE(snapshot.stats.empty());
}

// records used to be keyed on the `Job*`, so a job allocated where a finished one had been took
// over its record before anyone saw the first one finish
TEST_F(JobManagerTest, ReusedJobAddressesKeepSeparateRecords) {
  start_with_threads(1);
  manager->get_tracker().start_tracking();

  constexpr auto job_total = 64;
  for (auto i = 0; i < job_total; ++i) {
    manager->push_job_name(std::to_string(i));
    manager->submit(ox::Job::create([] {}));
    manager->pop_job_name();
    manager->wait();
  }

  auto snapshot = ox::JobSnapshot{};
  manager->get_tracker().snapshot(snapshot);
  EXPECT_TRUE(snapshot.live.empty());
  ASSERT_EQ(snapshot.history.size(), job_total);
  for (auto i = 0; i < job_total; ++i) {
    EXPECT_EQ(snapshot.history[i].name, std::to_string(i));
    EXPECT_EQ(snapshot.history[i].state, ox::JobState::Done);
  }

  auto names = std::vector<std::string>();
  manager->get_tracker().active_names(names);
  EXPECT_TRUE(names.empty());
}

TEST_F(JobManagerTest, ActiveNamesOutliveTheFirstOfSeveralSharingAName) {
  start_with_threads(2);
  manager->get_tracker().start_tracking();

  Gate first_gate;
  Gate second_gate;
  manager->push_job_name("Shared");
  manager->submit(ox::Job::create([&] { first_gate.wait(); }));
  manager->submit(ox::Job::create([&] { second_gate.wait(); }));
  manager->pop_job_name();

  auto names = std::vector<std::string>();
  manager->get_tracker().active_names(names);
  EXPECT_THAT(names, ::testing::ElementsAre("Shared"));

  first_gate.open();
  auto snapshot = ox::JobSnapshot{};
  ASSERT_TRUE(eventually([&] {
    manager->get_tracker().snapshot(snapshot);
    return snapshot.history.size() == 1;
  }));

  manager->get_tracker().active_names(names);
  EXPECT_THAT(names, ::testing::ElementsAre("Shared"));

  second_gate.open();
  manager->wait();

  manager->get_tracker().active_names(names);
  EXPECT_TRUE(names.empty());
}

TEST_F(JobManagerTest, RecordsParentAndWaitingForDrainedJobs) {
  // one worker, so the outer job's barrier wait has to run the inner job itself
  start_with_threads(1);
  manager->get_tracker().start_tracking();

  auto outer_id = ox::JobID::Invalid;
  auto inner_id = ox::JobID::Invalid;
  auto outer_state_inside = ox::JobState::Queued;
  auto inner_parent = ox::JobID::Invalid;

  manager->submit(ox::Job::create([&] {
    outer_id = ox::JobManager::current_job();

    auto barrier = ox::Barrier::create();
    auto inner = ox::Job::create([&] {
      inner_id = ox::JobManager::current_job();

      auto snapshot = ox::JobSnapshot{};
      manager->get_tracker().snapshot(snapshot);
      if (const auto* outer_record = find_record(snapshot.live, outer_id)) {
        outer_state_inside = outer_record->state;
      }

      if (const auto* inner_record = find_record(snapshot.live, inner_id)) {
        inner_parent = inner_record->parent;
      }
    });
    inner->signal(barrier);
    manager->submit(std::move(inner));
    barrier->wait(*manager);
  }));
  manager->wait();

  EXPECT_NE(outer_id, ox::JobID::Invalid);
  EXPECT_NE(inner_id, outer_id);
  EXPECT_EQ(outer_state_inside, ox::JobState::Waiting);
  EXPECT_EQ(inner_parent, outer_id);
  EXPECT_EQ(ox::JobManager::current_job(), ox::JobID::Invalid);
}

TEST_F(JobManagerTest, PrioritizeMovesQueuedJobToFront) {
  start_with_threads(1);

  Gate gate;
  auto blocker = ox::Job::create([&] { gate.wait(); });
  manager->submit(blocker);

  auto order = std::vector<int>();
  auto order_mutex = std::shared_mutex();
  auto queued = std::vector<ox::Arc<ox::Job>>();
  for (auto i = 0; i < 3; ++i) {
    auto job = ox::Job::create([&, i] {
      auto lock = std::unique_lock(order_mutex);
      order.push_back(i);
    });
    manager->submit(job);
    queued.push_back(std::move(job));
  }

  ASSERT_TRUE(eventually([&] { return !manager->prioritize(blocker->id); }));
  EXPECT_TRUE(manager->prioritize(queued[2]->id));

  gate.open();
  manager->wait();

  EXPECT_EQ(order, std::vector<int>({2, 0, 1}));
  EXPECT_FALSE(manager->prioritize(queued[2]->id));
}

TEST_F(JobManagerTest, CancelDropsQueuedJobsWithoutBarriers) {
  start_with_threads(1);
  manager->get_tracker().start_tracking();

  Gate gate;
  auto blocker = ox::Job::create([&] { gate.wait(); });
  manager->submit(blocker);

  std::atomic<bool> cancelled_ran{false};
  manager->push_job_name("Cancelled");
  auto cancelled = ox::Job::create([&] { cancelled_ran = true; });
  manager->submit(cancelled);
  manager->pop_job_name();

  std::atomic<bool> barrier_job_ran{false};
  auto barrier = ox::Barrier::create();
  auto with_barrier = ox::Job::create([&] { barrier_job_ran = true; });
  with_barrier->signal(barrier);
  manager->submit(with_barrier);

  // the blocker has to be off the queue before cancelling it can be refused for being taken
  ASSERT_TRUE(eventually([&] { return !manager->prioritize(blocker->id); }));
  EXPECT_FALSE(manager->cancel(blocker->id));
  EXPECT_FALSE(manager->cancel(with_barrier->id));
  EXPECT_TRUE(manager->cancel(cancelled->id));
  EXPECT_FALSE(manager->cancel(cancelled->id));

  gate.open();
  barrier->wait(*manager);
  manager->wait();

  EXPECT_FALSE(cancelled_ran);
  EXPECT_TRUE(barrier_job_ran);

  auto snapshot = ox::JobSnapshot{};
  manager->get_tracker().snapshot(snapshot);
  EXPECT_TRUE(snapshot.live.empty());
  const auto* record = find_record(snapshot.history, cancelled->id);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->state, ox::JobState::Cancelled);

  auto stats = std::ranges::find(snapshot.stats, std::string_view("Cancelled"), &ox::JobStats::label);
  ASSERT_NE(stats, snapshot.stats.end());
  EXPECT_EQ(stats->cancelled, 1u);
  EXPECT_EQ(stats->completed, 0u);
}

TEST_F(JobManagerTest, UnnamedJobsReachStatsButNotHistory) {
  start_with_threads(2);
  manager->get_tracker().start_tracking();

  for (auto i = 0; i < 3; ++i) {
    manager->submit(ox::Job::create([] {}));
  }
  manager->wait();

  auto snapshot = ox::JobSnapshot{};
  manager->get_tracker().snapshot(snapshot);
  EXPECT_TRUE(snapshot.history.empty());
  ASSERT_EQ(snapshot.stats.size(), 1);
  EXPECT_THAT(snapshot.stats[0].label, ::testing::StartsWith("TestJobManager.cpp:"));
  EXPECT_EQ(snapshot.stats[0].completed, 3u);
}

TEST_F(JobManagerTest, StopTrackingForgetsEverything) {
  start_with_threads(2);
  manager->get_tracker().start_tracking();

  manager->push_job_name("Forgotten");
  manager->submit(ox::Job::create([] {}));
  manager->pop_job_name();
  manager->wait();

  manager->get_tracker().stop_tracking();

  auto snapshot = ox::JobSnapshot{};
  manager->get_tracker().snapshot(snapshot);
  EXPECT_TRUE(snapshot.history.empty());
  EXPECT_TRUE(snapshot.stats.empty());
}
