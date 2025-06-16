#pragma once
#include <enkiTS/TaskScheduler.h>

#include "Core/ESystem.hpp"

namespace ox {
using TaskSet = enki::TaskSet;
using ITaskSet = enki::ITaskSet;
using IPinnedTask = enki::IPinnedTask;
using ICompleteableTask = enki::ICompletable;
using AsyncTask = enki::TaskSet;
using TaskSetPartition = enki::TaskSetPartition;
using TaskFunction = enki::TaskSetFunction;

class TaskScheduler : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  std::unique_ptr<enki::TaskScheduler>& get_underlying() { return task_scheduler; }

  void schedule_task(ITaskSet* set) const { task_scheduler->AddTaskSetToPipe(set); }

  void schedule_task(IPinnedTask* set) const { task_scheduler->AddPinnedTask(set); }

  void wait_task(const ITaskSet* set) const { task_scheduler->WaitforTask(set); }

  void wait_for_all();

private:
  std::unique_ptr<enki::TaskScheduler> task_scheduler;
  std::vector<std::unique_ptr<TaskSet>> task_sets = {};
};
} // namespace ox
