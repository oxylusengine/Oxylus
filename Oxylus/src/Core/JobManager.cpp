#include "Core/JobManager.hpp"

#include "Core/Base.hpp"
#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto Barrier::create() -> Arc<Barrier> { return Arc<Barrier>::create(); }

auto Barrier::wait(this Barrier& self, JobManager& job_manager) -> void {
  ZoneScoped;

  if (!self.sealed.test_and_set(std::memory_order_acq_rel)) {
    if (self.counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      self.counter.notify_all();
    }
  }

  // with no worker pool nothing else can drain the queue, so the waiter runs jobs itself instead of
  // blocking on a counter that will never reach zero
  const auto drain_here = this_thread_worker.id != ~0_u32 || job_manager.get_thread_count() == 0;
  auto v = self.counter.load(std::memory_order_acquire);
  while (v != 0) {
    if (drain_here) {
      if (!job_manager.try_execute_one()) {
        std::this_thread::yield();
      }
    } else {
      self.counter.wait(v, std::memory_order_acquire);
    }

    v = self.counter.load(std::memory_order_acquire);
  }
}

auto Job::signal(this Job& self, Arc<Barrier> barrier) -> Arc<Job> {
  ZoneScoped;

  barrier->counter.fetch_add(1, std::memory_order_relaxed);
  self.barriers.emplace_back(std::move(barrier));

  return &self;
}

auto JobManager::init() -> std::expected<void, std::string> {
  ZoneScoped;

  if (desired_thread_count == auto_thread_count) {
    unsigned int num_threads_available = std::thread::hardware_concurrency() - 1; // leave one for the OS
    num_threads = num_threads_available;
  } else {
    num_threads = desired_thread_count;
  }

  for (u32 i = 0; i < num_threads; i++) {
    this->workers.emplace_back([this, i]() { worker(i); });
  }

  return {};
}

auto JobManager::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  this->shutdown();

  return {};
}

auto JobManager::set_thread_count(this JobManager& self, u32 count) -> void {
  ZoneScoped;

  self.desired_thread_count = count;
}

auto JobManager::get_thread_count(this JobManager& self) -> u32 {
  ZoneScoped;

  return self.num_threads;
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
    auto job = Arc<Job>();
    {
      std::unique_lock lock(self.mutex);
      while (self.jobs.empty()) {
        if (!self.running) {
          return;
        }

        self.condition_var.wait(lock);
      }

      job = std::move(self.jobs.front());
      self.jobs.pop_front();
    }

    self.execute(std::move(job));
  }
}

auto JobManager::execute(this JobManager& self, Arc<Job> job) -> void {
  ZoneScoped;

  OX_DEFER(&) {
    for (auto& barrier : job->barriers) {
      if (barrier->counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        barrier->counter.notify_all();
      }
    }

    // Last, so `wait` cannot observe an idle pool while barriers are still being signalled.
    self.job_count.fetch_sub(1, std::memory_order_release);
  };

  job->task();
}

auto JobManager::try_execute_one(this JobManager& self) -> bool {
  ZoneScoped;

  auto job = Arc<Job>();
  {
    auto lock = std::unique_lock(self.mutex);
    if (self.jobs.empty()) {
      return false;
    }

    job = std::move(self.jobs.front());
    self.jobs.pop_front();
  }

  self.execute(std::move(job));

  return true;
}

auto JobManager::job_name_stack() -> std::stack<std::string>& {
  static thread_local std::stack<std::string> stack = {};
  return stack;
}

auto JobManager::submit(this JobManager& self, Arc<Job> job, bool prioritize) -> void {
  ZoneScoped;

  auto& name_stack = job_name_stack();
  if (self.tracker.is_tracking() && !name_stack.empty())
    job->name = name_stack.top();

  if (!job->name.empty()) {
    self.tracker.register_job(job);
    job->task = [original_task = std::move(job->task), job_ptr = job.get(), &t = self.tracker]() {
      original_task();
      t.mark_completed(job_ptr);
    };
  }

  {
    auto lock = std::unique_lock(self.mutex);
    if (prioritize) {
      self.jobs.push_front(std::move(job));
    } else {
      self.jobs.push_back(std::move(job));
    }
  }

  self.job_count.fetch_add(1);

  self.condition_var.notify_one();
}

auto JobManager::wait(this JobManager& self) -> void {
  ZoneScoped;

  // same as `Barrier::wait`: an uninitialized pool has to be drained by whoever waits on it, or this
  // spins forever
  const auto drain_here = this_thread_worker.id != ~0_u32 || self.num_threads == 0;
  while (self.job_count.load(std::memory_order_acquire) != 0) {
    if (!drain_here || !self.try_execute_one()) {
      std::this_thread::yield();
    }
  }
}
} // namespace ox
