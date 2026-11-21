#include "Core/JobManager.hpp"

#include <algorithm>
#include <utility>

#include "Core/Base.hpp"
#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "Utils/Log.hpp"

namespace ox {
static thread_local JobID current_job_id = JobID::Invalid;

// stats key: the job's name, or `File.cpp:123` for an unnamed one. the view points into `stack`
static auto stats_label(memory::ScopedStack& stack, const JobRecord& record) -> std::string_view {
  if (!record.name.empty()) {
    return record.name;
  }

  auto file = std::string_view(record.location.file_name());
  if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
    file.remove_prefix(slash + 1);
  }

  return stack.format("{}:{}", file, record.location.line());
}

auto JobTracker::start_tracking(this JobTracker& self) -> void {
  self.tracking_enabled.store(true, std::memory_order_relaxed);
}

auto JobTracker::stop_tracking(this JobTracker& self) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.mutex);
  self.tracking_enabled.store(false, std::memory_order_relaxed);
  self.live.clear();
  self.live_name_counts.clear();
  self.history.clear();
  self.stats.clear();
}

auto JobTracker::is_tracking(this const JobTracker& self) -> bool {
  return self.tracking_enabled.load(std::memory_order_relaxed);
}

auto JobTracker::clear_history(this JobTracker& self) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.mutex);
  self.history.clear();
  self.stats.clear();
}

auto JobTracker::snapshot(this const JobTracker& self, JobSnapshot& out) -> void {
  ZoneScoped;

  out.live.clear();
  out.history.clear();
  out.stats.clear();

  {
    auto lock = std::shared_lock(self.mutex);
    out.live.reserve(self.live.size());
    for (const auto& [id, record] : self.live) {
      out.live.push_back(record);
    }

    out.history.assign(self.history.begin(), self.history.end());
    out.stats.reserve(self.stats.size());
    for (const auto& [label, stats] : self.stats) {
      out.stats.push_back(stats);
    }
  }

  // ids are handed out in submit order, and erasing from the map reorders what is left
  std::ranges::sort(out.live, {}, [](const JobRecord& record) { return std::to_underlying(record.id); });
}

auto JobTracker::active_names(this const JobTracker& self, std::vector<std::string>& out) -> void {
  ZoneScoped;

  out.clear();

  auto lock = std::shared_lock(self.mutex);
  out.reserve(self.live_name_counts.size());
  for (const auto& [name, count] : self.live_name_counts) {
    out.push_back(name);
  }
}

auto JobTracker::count_stalled(this const JobTracker& self, const std::chrono::steady_clock::duration threshold)
  -> u32 {
  ZoneScoped;

  const auto now = std::chrono::steady_clock::now();
  auto stalled = 0_u32;

  auto lock = std::shared_lock(self.mutex);
  for (const auto& [id, record] : self.live) {
    if (record.state != JobState::Queued && now - record.started_at > threshold) {
      stalled += 1;
    }
  }

  return stalled;
}

auto JobTracker::on_submitted(this JobTracker& self, const Job& job) -> void {
  if (!self.is_tracking()) {
    return;
  }

  ZoneScoped;

  auto record = JobRecord{
    .id = job.id,
    .name = job.name,
    .location = job.location,
    .barrier_count = static_cast<u32>(job.barriers.size()),
    .submitted_at = std::chrono::steady_clock::now(),
  };

  auto lock = std::unique_lock(self.mutex);
  if (!record.name.empty()) {
    auto [it, is_inserted] = self.live_name_counts.try_emplace(record.name, 0_u32);
    it->second += 1;
  }

  self.live.insert_or_assign(job.id, std::move(record));
}

auto JobTracker::on_started(this JobTracker& self, const JobID id, const JobID parent, const u32 worker) -> void {
  if (!self.is_tracking()) {
    return;
  }

  ZoneScoped;

  const auto now = std::chrono::steady_clock::now();

  auto lock = std::unique_lock(self.mutex);
  auto it = self.live.find(id);
  if (it == self.live.end()) {
    return;
  }

  it->second.state = JobState::Running;
  it->second.parent = parent;
  it->second.worker = worker;
  it->second.started_at = now;
}

auto JobTracker::on_waiting(this JobTracker& self, const JobID id, const bool is_waiting) -> void {
  if (id == JobID::Invalid || !self.is_tracking()) {
    return;
  }

  ZoneScoped;

  const auto now = std::chrono::steady_clock::now();

  auto lock = std::unique_lock(self.mutex);
  auto it = self.live.find(id);
  if (it == self.live.end()) {
    return;
  }

  it->second.state = is_waiting ? JobState::Waiting : JobState::Running;
  if (is_waiting) {
    it->second.waiting_since = now;
  }
}

auto JobTracker::on_finished(this JobTracker& self, const JobID id, const JobState state) -> void {
  if (!self.is_tracking()) {
    return;
  }

  ZoneScoped;
  memory::ScopedStack stack;

  const auto now = std::chrono::steady_clock::now();

  auto lock = std::unique_lock(self.mutex);
  auto it = self.live.find(id);
  if (it == self.live.end()) {
    return;
  }

  auto record = std::move(it->second);
  self.live.erase(it);

  record.state = state;
  record.finished_at = now;

  if (!record.name.empty()) {
    if (
      auto count_it = self.live_name_counts.find(std::string_view(record.name)); count_it != self.live_name_counts.end()
    ) {
      count_it->second -= 1;
      if (count_it->second == 0) {
        self.live_name_counts.erase(count_it);
      }
    }
  }

  const auto label = stats_label(stack, record);
  auto stats_it = self.stats.find(label);
  if (stats_it == self.stats.end()) {
    stats_it = self.stats
                 .try_emplace(std::string(label), JobStats{.label = std::string(label), .location = record.location})
                 .first;
  }

  auto& stats = stats_it->second;
  stats.last_finished_at = now;
  if (state == JobState::Cancelled) {
    stats.cancelled += 1;
  } else {
    const auto run = std::chrono::duration_cast<std::chrono::nanoseconds>(now - record.started_at);
    const auto queued = std::chrono::duration_cast<std::chrono::nanoseconds>(record.started_at - record.submitted_at);
    stats.completed += 1;
    stats.total_run += run;
    stats.max_run = std::max(stats.max_run, run);
    stats.max_queued = std::max(stats.max_queued, queued);
  }

  if (record.name.empty()) {
    return;
  }

  if (self.history.size() >= JobTracker::HISTORY_CAPACITY) {
    self.history.pop_front();
  }

  self.history.push_back(std::move(record));
}

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
  if (v == 0) {
    return;
  }

  // the waiting job is current again whenever a drained one returns, so it is marked once here
  // rather than around every `try_execute_one`
  const auto waiting_job = JobManager::current_job();
  auto& tracker = job_manager.get_tracker();
  tracker.on_waiting(waiting_job, true);
  OX_DEFER(&) { tracker.on_waiting(waiting_job, false); };

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

  {
    std::unique_lock lock(self.mutex);
    self.running = false;
    self.condition_var.notify_all();
  }

  // `workers` is declared before `jobs`, `mutex` and `condition_var`, so the destructor would join
  // the threads only after everything they touch is already gone
  self.workers.clear();
  self.num_threads = 0;
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

  const auto parent_job = current_job_id;
  current_job_id = job->id;
  self.tracker.on_started(job->id, parent_job, this_thread_worker.id);

  OX_DEFER(&) {
    current_job_id = parent_job;
    // ahead of the barriers, so a woken waiter never sees its job still running
    self.tracker.on_finished(job->id, JobState::Done);

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

auto JobManager::prioritize(this JobManager& self, const JobID id) -> bool {
  ZoneScoped;

  auto lock = std::unique_lock(self.mutex);
  auto it = std::ranges::find_if(self.jobs, [id](const Arc<Job>& job) { return job->id == id; });
  if (it == self.jobs.end()) {
    return false;
  }

  auto job = std::move(*it);
  self.jobs.erase(it);
  self.jobs.push_front(std::move(job));

  return true;
}

auto JobManager::cancel(this JobManager& self, const JobID id) -> bool {
  ZoneScoped;

  // released after the lock, the closure's captures can run arbitrary destructors
  auto job = Arc<Job>();
  {
    auto lock = std::unique_lock(self.mutex);
    auto it = std::ranges::find_if(self.jobs, [id](const Arc<Job>& queued) { return queued->id == id; });
    if (it == self.jobs.end() || !(*it)->barriers.empty()) {
      return false;
    }

    job = std::move(*it);
    self.jobs.erase(it);
  }

  self.tracker.on_finished(id, JobState::Cancelled);
  self.job_count.fetch_sub(1, std::memory_order_release);

  return true;
}

auto JobManager::current_job() -> JobID { return current_job_id; }

auto JobManager::job_name_stack() -> std::stack<std::string>& {
  static thread_local std::stack<std::string> stack = {};
  return stack;
}

auto JobManager::submit(this JobManager& self, Arc<Job> job, bool prioritize) -> void {
  ZoneScoped;

  job->id = static_cast<JobID>(self.next_job_id.fetch_add(1, std::memory_order_relaxed));

  auto& name_stack = job_name_stack();
  if (self.tracker.is_tracking() && !name_stack.empty())
    job->name = name_stack.top();

  // before the push, a worker can start the job the moment it is queued
  self.tracker.on_submitted(*job);

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
