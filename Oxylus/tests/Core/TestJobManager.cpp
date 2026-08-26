#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

TEST_F(JobManagerTest, TracksJobStatusWhenEnabled) {
  start_with_threads(4);

  manager->get_tracker().start_tracking();

  // the job parks until the status below has been read, otherwise a worker can finish it first and
  // there is no "working" state left to observe
  std::atomic<bool> release{false};

  manager->push_job_name("TrackedJob");
  auto job = ox::Job::create([&] { release.wait(false, std::memory_order_acquire); });
  manager->submit(job);

  auto status = manager->get_tracker().get_status();
  ASSERT_EQ(status.size(), 1);
  EXPECT_EQ(status[0].first, "TrackedJob");
  EXPECT_TRUE(status[0].second); // Should be working
  manager->pop_job_name();

  release.store(true, std::memory_order_release);
  release.notify_all();

  manager->wait();

  status = manager->get_tracker().get_status();
  EXPECT_FALSE(status[0].second); // Should be done
}

TEST_F(JobManagerTest, NoTrackingWhenDisabled) {
  start_with_threads(4);

  manager->get_tracker().stop_tracking();

  manager->push_job_name("Untracked");
  manager->submit(ox::Job::create([] {}));
  manager->pop_job_name();
  manager->wait();

  EXPECT_TRUE(manager->get_tracker().get_status().empty());
}

TEST_F(JobManagerTest, CleanupOldJobs) {
  start_with_threads(4);

  manager->get_tracker().start_tracking();

  const std::string job_name = "TempJob";
  manager->push_job_name(job_name);
  auto job = ox::Job::create([] {});
  manager->submit(job);
  manager->pop_job_name();
  manager->wait();

  // Simulate time passing
  if (auto record = manager->get_tracker().find_job(job_name)) {
    record->get().completion_time -= std::chrono::seconds(3);

    // Cleanup jobs older than 1 second
    manager->get_tracker().cleanup_old(std::chrono::seconds(1));

    // Verify the job was cleaned up
    EXPECT_TRUE(manager->get_tracker().get_status().empty());
  } else {
    FAIL() << "Job record not found for: " << job_name;
  }
}
