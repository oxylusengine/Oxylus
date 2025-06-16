#include "Thread.hpp"

namespace ox {
Thread::Thread() { worker = std::thread(&Thread::queue_loop, this); }

Thread::~Thread() {
  if (worker.joinable()) {
    wait();
    queue_mutex.lock();
    destroying = true;
    condition.notify_one();
    queue_mutex.unlock();
    worker.join();
  }
}

void Thread::queue_job(std::function<void()> function) {
  std::lock_guard lock(queue_mutex);
  job_queue.push(std::move(function));
  condition.notify_one();
}

void Thread::wait() {
  std::unique_lock lock(queue_mutex);
  condition.wait(lock, [this]() { return job_queue.empty(); });
}

void Thread::queue_loop() {
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock lock(queue_mutex);
      condition.wait(lock, [this] { return !job_queue.empty() || destroying; });
      if (destroying) {
        break;
      }
      job = job_queue.front();
    }

    job();

    {
      std::lock_guard lock(queue_mutex);
      job_queue.pop();
      condition.notify_one();
    }
  }
}
} // namespace ox
