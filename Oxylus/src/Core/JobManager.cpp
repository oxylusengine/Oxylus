#include "Core/JobManager.hpp"

#include "Core/Base.hpp"
#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto Barrier::create() -> Arc<Barrier> { return Arc<Barrier>::create(); }

auto Barrier::wait(this Barrier& self) -> void {
  auto v = self.counter.load();
  while (v != 0) {
    self.counter.wait(v);
    v = self.counter.load();
  }
}

auto Barrier::acquire(this Barrier& self, u32 count) -> Arc<Barrier> {
  self.counter += count;
  self.acquired += count;

  return &self;
}

auto Barrier::add(this Barrier& self, Arc<Job> job) -> Arc<Barrier> {
  self.pending.push_back(std::move(job));

  return &self;
}

auto Job::signal(this Job& self, Arc<Barrier> barrier) -> Arc<Job> {
  ZoneScoped;

  if (barrier->acquired > 0) {
    barrier->acquired--;
  } else {
    barrier->acquired++;
  }

  self.barriers.emplace_back(std::move(barrier));
  return &self;
}

auto JobManager::init() -> std::expected<void, std::string> {
  ZoneScoped;

  return {};
}

auto JobManager::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  this->shutdown();

  return {};
}

JobManager::JobManager(u32 threads) {
  ZoneScoped;

  if (threads == auto_thread_count) {
    unsigned int num_threads_available = std::thread::hardware_concurrency() - 1; // leave one for the OS
    num_threads = num_threads_available;
  } else {
    num_threads = threads;
  }

  for (u32 i = 0; i < num_threads; i++) {
    this->workers.emplace_back([this, i]() { worker(i); });
  }
}

auto JobManager::shutdown(this JobManager& self) -> void {
  ZoneScoped;

  std::unique_lock lock(self.mutex);
  self.running = false;
  self.condition_var.notify_all();
}

auto JobManager::worker(this JobManager& self, u32 id) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  this_thread_worker.id = id;
  os::set_thread_name(stack.format("Worker {}", id));
  loguru::set_thread_name(stack.format_char("Worker {}", id));

  OX_DEFER() { this_thread_worker.id = ~0_u32; };

  while (true) {
    std::unique_lock lock(self.mutex);
    while (self.jobs.empty()) {
      if (!self.running) {
        return;
      }

      self.condition_var.wait(lock);
    }

    auto job = self.jobs.front();
    self.jobs.pop_front();
    lock.unlock();

    job->task();
    self.job_count.fetch_sub(1);

    for (auto& barrier : job->barriers) {
      if (--barrier->counter == 0) {
        for (auto& task : barrier->pending) {
          self.submit(task, true);
        }

        barrier->counter.notify_all();
      }
    }
  }
}

auto JobManager::submit(this JobManager& self, Arc<Job> job, bool prioritize) -> void {
  ZoneScoped;

  if (!self.job_name_stack.empty())
    job->name = self.job_name_stack.top();

  self.tracker.register_job(job);

  job->task = [original_task = std::move(job->task), job_ptr = job.get(), &t = self.tracker]() {
    original_task();
    t.mark_completed(job_ptr);
  };

  {
    std::unique_lock lock(self.mutex);
    if (prioritize) {
      self.jobs.push_front(std::move(job));
    } else {
      self.jobs.push_back(std::move(job));
    }
  }

  self.job_count.fetch_add(1);
  self.condition_var.notify_all();
}

auto JobManager::wait(this JobManager& self) -> void {
  ZoneScoped;

  while (self.job_count.load(std::memory_order_relaxed) != 0)
    ;
}
} // namespace ox
