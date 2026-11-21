#pragma once

#include <ankerl/unordered_dense.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <stack>
#include <string>
#include <thread>
#include <vector>

#include "Core/Arc.hpp"
#include "Core/Option.hpp"

namespace ox {
using JobFn = std::function<void()>;

struct Job;
class JobManager;

enum class JobID : u64 { Invalid = ~0_u64 };
enum class JobState : u8 { Queued = 0, Running, Waiting, Done, Cancelled };

struct Barrier : ManagedObj {
  // Pinned at 1 until the first `wait`, so the counter cannot transiently hit zero while jobs are
  // still being fanned out.
  std::atomic<u32> counter = 1;
  std::atomic_flag sealed = {};

  static auto create() -> Arc<Barrier>;
  auto wait(this Barrier& self, JobManager& job_manager) -> void;
};

struct Job : ManagedObj {
  std::vector<Arc<Barrier>> barriers = {};
  JobFn task = {};

  std::string name = {};     // managed with push/pop
  std::source_location location = {};
  JobID id = JobID::Invalid; // handed out by `submit`
  std::atomic<bool> is_done{false};

  template <typename Fn>
  static auto create(Fn&& task, std::source_location location = std::source_location::current()) -> Arc<Job> {
    auto job = Arc<Job>::create();
    job->location = location;
    job->task = [task_fn = std::forward<Fn>(task), done_flag = &job->is_done]() {
      task_fn();
      done_flag->store(true, std::memory_order_release);
    };
    return job;
  }

  auto signal(this Job& self, Arc<Barrier> barrier) -> Arc<Job>;
};

struct JobRecord {
  JobID id = JobID::Invalid;
  // the job that was already running on the same thread, when a barrier wait drained this one
  JobID parent = JobID::Invalid;
  std::string name = {};
  std::source_location location = {};
  JobState state = JobState::Queued;
  u32 worker = ~0_u32; // ~0 when the main thread or a thread outside the pool ran it
  u32 barrier_count = 0;
  std::chrono::steady_clock::time_point submitted_at = {};
  std::chrono::steady_clock::time_point started_at = {};
  std::chrono::steady_clock::time_point waiting_since = {};
  std::chrono::steady_clock::time_point finished_at = {};
};

// grouped by name, or by source location for unnamed jobs
struct JobStats {
  std::string label = {};
  std::source_location location = {};
  u64 completed = 0;
  u64 cancelled = 0;
  std::chrono::nanoseconds total_run = {};
  std::chrono::nanoseconds max_run = {};
  std::chrono::nanoseconds max_queued = {};
  std::chrono::steady_clock::time_point last_finished_at = {};
};

struct JobSnapshot {
  // queued, running and waiting jobs, oldest first
  std::vector<JobRecord> live = {};
  // finished named jobs, oldest first; unnamed ones only reach `stats`, or physics floods it
  std::vector<JobRecord> history = {};
  std::vector<JobStats> stats = {};
};

class JobTracker {
public:
  static constexpr usize HISTORY_CAPACITY = 512;

  auto start_tracking(this JobTracker& self) -> void;
  // also forgets everything, a record left behind would never see its job finish
  auto stop_tracking(this JobTracker& self) -> void;
  auto is_tracking(this const JobTracker& self) -> bool;
  auto clear_history(this JobTracker& self) -> void;

  auto snapshot(this const JobTracker& self, JobSnapshot& out) -> void;
  // names with at least one queued or running job
  auto active_names(this const JobTracker& self, std::vector<std::string>& out) -> void;
  // running or waiting jobs started longer than `threshold` ago; long queues are normal and not counted
  auto count_stalled(this const JobTracker& self, std::chrono::steady_clock::duration threshold) -> u32;

  auto on_submitted(this JobTracker& self, const Job& job) -> void;
  auto on_started(this JobTracker& self, JobID id, JobID parent, u32 worker) -> void;
  auto on_waiting(this JobTracker& self, JobID id, bool is_waiting) -> void;
  auto on_finished(this JobTracker& self, JobID id, JobState state) -> void;

private:
  struct LabelHash {
    using is_transparent = void;
    using is_avalanching = void;
    auto operator()(this const LabelHash&, std::string_view label) noexcept -> u64 {
      return ankerl::unordered_dense::hash<std::string_view>{}(label);
    }
  };

  mutable std::shared_mutex mutex = {};
  ankerl::unordered_dense::map<JobID, JobRecord> live = {};
  ankerl::unordered_dense::map<std::string, u32, LabelHash, std::equal_to<>> live_name_counts = {};
  std::deque<JobRecord> history = {};
  ankerl::unordered_dense::map<std::string, JobStats, LabelHash, std::equal_to<>> stats = {};
  std::atomic<bool> tracking_enabled{false};
};

struct ThreadWorker {
  u32 id = ~0_u32;
};

inline thread_local ThreadWorker this_thread_worker;

class JobManager {
public:
  inline static const std::thread::id main_thread_id = std::this_thread::get_id();

  JobManager() = default;
  ~JobManager() = default;

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  static auto is_main_thread() noexcept -> bool { return std::this_thread::get_id() == main_thread_id; }

  auto set_thread_count(this JobManager& self, u32 count) -> void;
  auto get_thread_count(this JobManager& self) -> u32;

  auto shutdown(this JobManager& self) -> void;
  auto worker(this JobManager& self, u32 id) -> void;
  auto submit(this JobManager& self, Arc<Job> job, bool prioritize = false) -> void;
  auto wait(this JobManager& self) -> void;

  auto try_execute_one(this JobManager& self) -> bool;

  // moves a queued job to the front of the queue, false once a worker has taken it
  auto prioritize(this JobManager& self, JobID id) -> bool;
  // drops a queued job without running it. jobs that signal a barrier are refused, releasing their
  // waiters early would hand them unfinished work
  auto cancel(this JobManager& self, JobID id) -> bool;

  // the job running on the calling thread, innermost when a barrier wait is draining others
  static auto current_job() -> JobID;

  static auto job_name_stack() -> std::stack<std::string>&;

  auto push_job_name(this JobManager&, std::string_view name) -> void { job_name_stack().emplace(name); }
  auto pop_job_name(this JobManager&) -> void {
    auto& stack = job_name_stack();
    if (!stack.empty()) {
      stack.pop();
    }
  }

  auto get_tracker(this JobManager& self) -> JobTracker& { return self.tracker; }

  template <std::input_iterator Iterator, typename Func>
  auto for_each(
    this JobManager& self,
    Iterator begin,
    Iterator end,
    Func&& func,
    std::source_location location = std::source_location::current()
  ) -> void {
    const usize total_size = std::distance(begin, end);
    if (total_size == 0)
      return;

    const usize chunk_size = std::max<usize>(1, total_size / (self.num_threads * 4));
    usize global_index = 0;

    for (auto chunk_start = begin; chunk_start != end;) {
      auto chunk_end = chunk_start;
      const usize chunk_start_index = global_index;
      std::advance(chunk_end, std::min(chunk_size, static_cast<usize>(std::distance(chunk_start, end))));
      global_index += std::distance(chunk_start, chunk_end);

      self.submit(
        Job::create(
          [=, f = std::forward_like<Func>(func)] {
            usize local_index = chunk_start_index;
            for (auto it = chunk_start; it != chunk_end; ++it, ++local_index) {
              std::invoke(f, *it, local_index);
            }
          },
          location
        )
      );

      chunk_start = chunk_end;
    }
  }

  template <std::ranges::contiguous_range Range, typename Func>
  auto for_each(
    this JobManager& self, Range&& range, Func&& func, std::source_location location = std::source_location::current()
  ) -> void {
    auto view = std::span(std::forward<Range>(range));
    self.for_each(view.begin(), view.end(), std::forward<Func>(func), location);
  }

  template <typename T>
  struct AsyncDataHolder : ManagedObj {
    T data;
    std::atomic<u32> active_jobs{0};

    explicit AsyncDataHolder(const T& src) : data(src) {}
    explicit AsyncDataHolder(T&& src) : data(std::move(src)) {}
  };

  struct AsyncCompletionToken : ManagedObj {
    std::function<void()> callback;
    std::atomic<u32> pending_jobs{0};

    static auto create() -> Arc<AsyncCompletionToken> { return Arc<AsyncCompletionToken>::create(); }
  };

  template <std::ranges::contiguous_range T, typename Func>
  void for_each_async(this JobManager& self, T& vec, Func&& func, std::function<void()>&& completion_callback = {}) {
    auto token = AsyncCompletionToken::create();
    token->callback = completion_callback;
    auto holder = Arc<AsyncDataHolder<T>>::create(vec);

    const size_t chunk_size = std::max<size_t>(1, vec.size() / (self.num_threads * 4));

    for (size_t start = 0; start < vec.size(); start += chunk_size) {
      const size_t end = std::min(start + chunk_size, vec.size());

      holder->acquire_ref();
      holder->active_jobs.fetch_add(1, std::memory_order_relaxed);
      token->pending_jobs.fetch_add(1, std::memory_order_relaxed);

      self.submit(Job::create([&self, token, holder, start, end, f = std::forward<Func>(func)]() {
        for (size_t i = start; i < end; ++i) {
          f(holder->data[i], i);
        }

        if (holder->active_jobs.fetch_sub(1, std::memory_order_release) == 1) {
          holder->release_ref();
          if (token->callback) {
            self.push_job_name("Completion callback");
            self.submit(Job::create([t = std::move(token)]() { t->callback(); }));
            self.pop_job_name();
          }
        }
      }));
    }

    holder->release_ref();
  }

private:
  auto execute(this JobManager& self, Arc<Job> job) -> void;

  JobTracker tracker = {};

  static constexpr u32 auto_thread_count = 0;
  u32 desired_thread_count = auto_thread_count;

  u32 num_threads = 0;
  std::vector<std::jthread> workers = {};
  std::deque<Arc<Job>> jobs = {};
  std::shared_mutex mutex = {};
  std::condition_variable_any condition_var = {};
  std::atomic<u64> job_count = {};
  std::atomic<u64> next_job_id = {};
  bool running = true;
};

} // namespace ox
