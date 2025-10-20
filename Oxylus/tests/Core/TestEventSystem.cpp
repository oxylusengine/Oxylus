#include <array>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <latch>
#include <thread>

#include "Core/EventSystem.hpp"


class EventSystemTest : public ::testing::Test {
protected:
  void SetUp() override {
    event_system = std::make_unique<ox::EventSystem>();
    auto init_result = event_system->init();
    EXPECT_TRUE(init_result.has_value());
  }

  void TearDown() override {
    auto deinit_result = event_system->deinit();
    EXPECT_TRUE(deinit_result.has_value());
  }

  std::unique_ptr<ox::EventSystem> event_system = nullptr;
};

TEST_F(EventSystemTest, SubscribeAndUnsubscribe) {
  struct TestEvent {
    int x, y;
  };

  auto sub = event_system->subscribe<TestEvent>([](const TestEvent& e) {});
  EXPECT_TRUE(sub.has_value());
  EXPECT_EQ(event_system->handler_count<TestEvent>(), 1);

  auto unsub = event_system->unsubscribe<TestEvent>(sub.value());
  EXPECT_TRUE(unsub.has_value());
  EXPECT_EQ(event_system->handler_count<TestEvent>(), 0);
}

TEST_F(EventSystemTest, SubscribeAndEmitEvent) {
  static constexpr auto x_value = 100;
  static constexpr auto y_value = 200;

  struct TestEvent {
    int x, y;
  };

  std::atomic<bool> event_called{false};

  auto sub = event_system->subscribe<TestEvent>([&event_called](const TestEvent& e) {
    event_called = true;
    EXPECT_EQ(e.x, x_value);
    EXPECT_EQ(e.y, y_value);
  });

  auto _ = event_system->emit(TestEvent{x_value, y_value});

  EXPECT_TRUE(event_called);
}

TEST_F(EventSystemTest, SubscribeAndEmitEventThreads) {
  static constexpr auto x_value = 100;
  static constexpr auto y_value = 200;

  struct TestEvent {
    int id, x, y;
  };

  static constexpr auto emit_count = 10;

  std::array<std::atomic<bool>, emit_count> events_called = {};

  // Synchronization to ensure subscriber is ready before emitting
  std::atomic<bool> subscriber_ready{false};
  std::latch emit_latch(1);

  std::vector<std::jthread> threads = {};

  // Subscriber thread
  threads.emplace_back([&events_called, &subscriber_ready, &emit_latch, es = this->event_system.get()] {
    auto sub = es->subscribe<TestEvent>([&events_called](const TestEvent& e) {
      if (e.id >= 0 && e.id < emit_count) {
        events_called[e.id] = true;
      }
    });

    // Signal that subscriber is ready
    subscriber_ready.store(true, std::memory_order_release);
    emit_latch.count_down();

    // Keep subscription alive until thread ends
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });

  // Emitter thread
  threads.emplace_back([&emit_latch, es = this->event_system.get()] {
    emit_latch.wait(); // Wait for subscriber to be ready

    for (int i = 0; i < emit_count; ++i) {
      auto _ = es->emit(TestEvent{i, i * x_value, i * y_value});
    }
  });

  for (auto& t : threads) {
    t.join();
  }

  for (size_t i = 0; i < events_called.size(); ++i) {
    EXPECT_TRUE(events_called[i]) << "Event " << i << " was not received";
  }
}

TEST_F(EventSystemTest, ConcurrentSubscribeEmitUnsubscribe) {
  struct TestEvent {
    int value;
  };

  std::atomic<int> total_received{0};
  std::atomic<bool> stop{false};

  auto subscriber_fn = [&, es = this->event_system.get()] {
    for (int i = 0; i < 50; ++i) {
      auto sub = es->subscribe<TestEvent>([&](const TestEvent&) {
        total_received.fetch_add(1, std::memory_order_relaxed);
      });
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  };

  auto emitter_fn = [&, es = this->event_system.get()] {
    for (int i = 0; i < 100 && !stop.load(); ++i) {
      auto _ = es->emit(TestEvent{i});
      std::this_thread::yield();
    }
  };

  std::vector<std::jthread> threads;
  threads.emplace_back(subscriber_fn);
  threads.emplace_back(subscriber_fn);
  threads.emplace_back(emitter_fn);
  threads.emplace_back(emitter_fn);

  for (auto& t : threads) {
    t.join();
  }

  stop.store(true);

  EXPECT_GT(total_received.load(), 0);
}
