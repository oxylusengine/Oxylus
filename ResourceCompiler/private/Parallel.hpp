#pragma once

#include "Core/JobManager.hpp"

namespace ox::rc {
// fans work over a session's pool, or runs it inline when there is nothing to fan out to. the
// barrier is joined in the destructor, so a dispatched job may safely capture anything living in
// the enclosing scope
struct ParallelScope {
  JobManager& job_manager;
  Arc<Barrier> barrier = Barrier::create();
  bool inline_only = false;

  explicit ParallelScope(JobManager& job_manager_)
      : job_manager(job_manager_),
        inline_only(job_manager_.get_thread_count() <= 1) {}

  ~ParallelScope() { barrier->wait(job_manager); }

  ParallelScope(const ParallelScope&) = delete;
  auto operator=(const ParallelScope&) -> ParallelScope& = delete;

  template <typename Fn>
  auto dispatch(this ParallelScope& self, Fn&& work) -> void {
    if (self.inline_only) {
      work();
      return;
    }

    auto job = Job::create(std::forward<Fn>(work));
    job->signal(self.barrier);
    self.job_manager.submit(std::move(job));
  }
};
} // namespace ox::rc
