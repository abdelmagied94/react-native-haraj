/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RuntimeScheduler.h"

#include <utility>
#include "ErrorUtils.h"

namespace facebook {
namespace react {

#pragma mark - Public

RuntimeScheduler::RuntimeScheduler(
    RuntimeExecutor runtimeExecutor,
    std::function<RuntimeSchedulerTimePoint()> now)
    : runtimeExecutor_(std::move(runtimeExecutor)), now_(std::move(now)) {}

void RuntimeScheduler::scheduleWork(
    std::function<void(jsi::Runtime &)> callback) const {
  shouldYield_ = true;
  runtimeExecutor_(
      [this, callback = std::move(callback)](jsi::Runtime &runtime) {
        shouldYield_ = false;
        callback(runtime);
        startWorkLoop(runtime);
      });
}

std::shared_ptr<Task> RuntimeScheduler::scheduleTask(
    SchedulerPriority priority,
    jsi::Function callback) {
  auto expirationTime = now() + timeoutForSchedulerPriority(priority);
  auto task =
      std::make_shared<Task>(priority, std::move(callback), expirationTime);
  taskQueue_.push(task);

  scheduleWorkLoopIfNecessary();

  return task;
}

bool RuntimeScheduler::getShouldYield() const noexcept {
  return shouldYield_;
}

bool RuntimeScheduler::getIsSynchronous() const noexcept {
  return isSynchronous_;
}

void RuntimeScheduler::cancelTask(Task &task) noexcept {
  task.callback.reset();
}

SchedulerPriority RuntimeScheduler::getCurrentPriorityLevel() const noexcept {
  return currentPriority_;
}

RuntimeSchedulerTimePoint RuntimeScheduler::now() const noexcept {
  return now_();
}

void RuntimeScheduler::executeNowOnTheSameThread(
    std::function<void(jsi::Runtime &runtime)> callback) {
  shouldYield_ = true;
  executeSynchronouslyOnSameThread_CAN_DEADLOCK(
      runtimeExecutor_,
      [this, callback = std::move(callback)](jsi::Runtime &runtime) {
        shouldYield_ = false;
        isSynchronous_ = true;
        callback(runtime);
        isSynchronous_ = false;
      });

  // Resume work loop if needed. In synchronous mode
  // only expired tasks are executed. Tasks with lower priority
  // might be still in the queue.
  scheduleWorkLoopIfNecessary();
}

void RuntimeScheduler::callImmediates(jsi::Runtime &runtime) {
  auto previousPriority = currentPriority_;
  try {
    while (!taskQueue_.empty()) {
      auto topPriorityTask = taskQueue_.top();
      auto now = now_();
      auto didUserCallbackTimeout = topPriorityTask->expirationTime <= now;

      if (!didUserCallbackTimeout) {
        break;
      }

      currentPriority_ = topPriorityTask->priority;
      auto result = topPriorityTask->execute(runtime);

      if (result.isObject() && result.getObject(runtime).isFunction(runtime)) {
        topPriorityTask->callback =
            result.getObject(runtime).getFunction(runtime);
      } else {
        if (taskQueue_.top() == topPriorityTask) {
          taskQueue_.pop();
        }
      }
    }
  } catch (jsi::JSError &error) {
    handleFatalError(runtime, error);
  }

  currentPriority_ = previousPriority;
}

#pragma mark - Private

void RuntimeScheduler::scheduleWorkLoopIfNecessary() const {
  if (!isWorkLoopScheduled_ && !isPerformingWork_) {
    isWorkLoopScheduled_ = true;
    runtimeExecutor_([this](jsi::Runtime &runtime) {
      isWorkLoopScheduled_ = false;
      startWorkLoop(runtime);
    });
  }
}

void RuntimeScheduler::startWorkLoop(jsi::Runtime &runtime) const {
  auto previousPriority = currentPriority_;
  isPerformingWork_ = true;
  try {
    while (!taskQueue_.empty()) {
      auto topPriorityTask = taskQueue_.top();
      auto now = now_();
      auto didUserCallbackTimeout = topPriorityTask->expirationTime <= now;

      if (!didUserCallbackTimeout && getShouldYield()) {
        // This currentTask hasn't expired, and we need to yield.
        break;
      }

      currentPriority_ = topPriorityTask->priority;
      auto result = topPriorityTask->execute(runtime);

      if (result.isObject() && result.getObject(runtime).isFunction(runtime)) {
        topPriorityTask->callback =
            result.getObject(runtime).getFunction(runtime);
      } else {
        if (taskQueue_.top() == topPriorityTask) {
          taskQueue_.pop();
        }
      }
    }
  } catch (jsi::JSError &error) {
    handleFatalError(runtime, error);
  }

  currentPriority_ = previousPriority;
  isPerformingWork_ = false;
}

} // namespace react
} // namespace facebook
