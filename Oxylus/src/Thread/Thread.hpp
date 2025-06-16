#pragma once

namespace ox {
class Thread {
public:
  Thread();
  ~Thread();

  void queue_job(std::function<void()> function);
  void wait();
  uint32_t get_queue_size() const { return (uint32_t)job_queue.size(); }

private:
  void queue_loop();

  bool destroying = false;
  std::thread worker;
  std::queue<std::function<void()>> job_queue;
  std::mutex queue_mutex;
  std::condition_variable condition;
};
} // namespace ox
