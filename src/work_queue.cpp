#include "sanguinius/work_queue.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sanguinius {

class BoundedExecutor::Impl {
public:
  Impl(const std::size_t capacity, const std::size_t worker_count)
      : capacity_{capacity}, worker_count_{worker_count} {
    if (capacity_ == 0) {
      throw std::invalid_argument{"Queue capacity must be at least one."};
    }
    if (worker_count_ == 0) {
      throw std::invalid_argument{"Worker count must be at least one."};
    }
  }

  void start() {
    {
      const std::scoped_lock lock{mutex_};
      if (started_) {
        throw std::logic_error{"Bounded executor may only be started once."};
      }
      started_ = true;
      accepting_ = true;
    }

    try {
      workers_.reserve(worker_count_);
      for (std::size_t index = 0; index < worker_count_; ++index) {
        static_cast<void>(index);
        workers_.emplace_back(
            [this](const std::stop_token stop_token) { run(stop_token); });
      }
    } catch (...) {
      stop();
      throw;
    }
  }

  [[nodiscard]] SubmitResult try_submit(Task task, Cancellation cancellation,
                                        const bool priority,
                                        const bool superseding = false) {
    Cancellation displaced;
    {
      const std::scoped_lock lock{mutex_};
      if (!accepting_) {
        return SubmitResult::stopping;
      }
      if (tasks_.size() >= capacity_) {
        if (!superseding)
          return SubmitResult::full;
        displaced = std::move(tasks_.back().cancellation);
        tasks_.pop_back();
      }
      PendingTask pending{.task = std::move(task),
                          .cancellation = std::move(cancellation)};
      if (priority)
        tasks_.push_front(std::move(pending));
      else
        tasks_.push_back(std::move(pending));
    }
    if (displaced) {
      try {
        displaced();
      } catch (...) {
      }
    }
    ready_.notify_one();
    return SubmitResult::accepted;
  }

  [[nodiscard]] QueueSnapshot snapshot() const {
    const std::scoped_lock lock{mutex_};
    return QueueSnapshot{
        .capacity = capacity_,
        .queued = tasks_.size(),
        .active = active_,
        .accepting = accepting_,
    };
  }

  void stop() noexcept {
    std::deque<PendingTask> pending;
    {
      const std::scoped_lock lock{mutex_};
      accepting_ = false;
      pending.swap(tasks_);
    }
    for (auto &task : pending) {
      if (!task.cancellation)
        continue;
      try {
        task.cancellation();
      } catch (...) {
      }
    }
    for (auto &worker : workers_) {
      worker.request_stop();
    }
    ready_.notify_all();
    workers_.clear();
  }

private:
  void run(const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
      Task task;
      {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this, stop_token] {
          return stop_token.stop_requested() || !tasks_.empty();
        });
        if (stop_token.stop_requested()) {
          return;
        }
        task = std::move(tasks_.front().task);
        tasks_.pop_front();
        ++active_;
      }

      try {
        task(stop_token);
      } catch (...) {
      }
      {
        const std::scoped_lock lock{mutex_};
        --active_;
      }
    }
  }

  std::size_t capacity_;
  std::size_t worker_count_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  struct PendingTask {
    Task task;
    Cancellation cancellation;
  };

  std::deque<PendingTask> tasks_;
  std::vector<std::jthread> workers_;
  bool started_{false};
  bool accepting_{false};
  std::size_t active_{};
};

BoundedExecutor::BoundedExecutor(const std::size_t capacity,
                                 const std::size_t worker_count)
    : impl_{std::make_unique<Impl>(capacity, worker_count)} {}

BoundedExecutor::~BoundedExecutor() { stop(); }

void BoundedExecutor::start() { impl_->start(); }

SubmitResult BoundedExecutor::try_submit(Task task, Cancellation cancellation) {
  return impl_->try_submit(std::move(task), std::move(cancellation), false);
}

SubmitResult BoundedExecutor::try_submit_front(Task task,
                                               Cancellation cancellation) {
  return impl_->try_submit(std::move(task), std::move(cancellation), true);
}

SubmitResult
BoundedExecutor::try_submit_front_superseding(Task task,
                                              Cancellation cancellation) {
  return impl_->try_submit(std::move(task), std::move(cancellation), true,
                           true);
}

QueueSnapshot BoundedExecutor::snapshot() const { return impl_->snapshot(); }

void BoundedExecutor::stop() noexcept { impl_->stop(); }

} // namespace sanguinius
