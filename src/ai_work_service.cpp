#include "sanguinius/ai_work_service.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sanguinius {
namespace {

enum class Lane { direct, explicit_feature, optional };

struct PendingAiTask {
  BoundedExecutor::Task task;
  BoundedExecutor::Cancellation cancellation;
};

} // namespace

class AiWorkService::Impl {
public:
  Impl(const std::size_t capacity, const std::size_t worker_count)
      : capacity_{capacity}, worker_count_{worker_count} {
    if (capacity_ == 0 || worker_count_ == 0)
      throw std::invalid_argument{
          "AI queue needs at least one slot and worker."};
  }

  void start() {
    {
      const std::scoped_lock lock{mutex_};
      if (started_)
        throw std::logic_error{"AI work service may only be started once."};
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

  SubmitResult submit(const Lane lane, BoundedExecutor::Task task,
                      BoundedExecutor::Cancellation cancellation) {
    {
      const std::scoped_lock lock{mutex_};
      if (!accepting_)
        return SubmitResult::stopping;
      const auto total = queued_unlocked();
      if (total >= capacity_)
        return SubmitResult::full;
      const auto nondirect = explicit_.size() + optional_.size();
      // The production queue has 64 slots and reserves sixteen for direct
      // interaction work. Small injected queues remain useful for deterministic
      // saturation tests; they cannot reserve capacity they do not have.
      const auto nondirect_capacity =
          capacity_ > 16 ? capacity_ - 16 : capacity_;
      if (lane != Lane::direct && nondirect >= nondirect_capacity)
        return SubmitResult::full;
      if (lane == Lane::explicit_feature && nondirect >= 48)
        return SubmitResult::full;
      if (lane == Lane::optional && (optional_.size() >= 32 || nondirect >= 48))
        return SubmitResult::full;
      queue(lane).push_back(
          {.task = std::move(task), .cancellation = std::move(cancellation)});
    }
    ready_.notify_one();
    return SubmitResult::accepted;
  }

  QueueSnapshot snapshot() const {
    const std::scoped_lock lock{mutex_};
    return {.capacity = capacity_,
            .queued = queued_unlocked(),
            .active = active_,
            .accepting = accepting_};
  }

  void stop() noexcept {
    std::deque<PendingAiTask> pending;
    {
      const std::scoped_lock lock{mutex_};
      accepting_ = false;
      append(pending, direct_);
      append(pending, explicit_);
      append(pending, optional_);
    }
    for (auto &item : pending) {
      if (!item.cancellation)
        continue;
      try {
        item.cancellation();
      } catch (...) {
      }
    }
    for (auto &worker : workers_)
      worker.request_stop();
    ready_.notify_all();
    workers_.clear();
  }

private:
  static void append(std::deque<PendingAiTask> &target,
                     std::deque<PendingAiTask> &source) {
    while (!source.empty()) {
      target.push_back(std::move(source.front()));
      source.pop_front();
    }
  }

  [[nodiscard]] std::size_t queued_unlocked() const noexcept {
    return direct_.size() + explicit_.size() + optional_.size();
  }

  [[nodiscard]] std::deque<PendingAiTask> &queue(const Lane lane) noexcept {
    if (lane == Lane::direct)
      return direct_;
    if (lane == Lane::explicit_feature)
      return explicit_;
    return optional_;
  }

  [[nodiscard]] Lane next_lane() noexcept {
    constexpr std::array schedule{
        Lane::direct,  Lane::direct,           Lane::direct,
        Lane::direct,  Lane::explicit_feature, Lane::explicit_feature,
        Lane::optional};
    for (std::size_t checked = 0; checked < schedule.size(); ++checked) {
      const auto candidate = schedule[schedule_index_];
      schedule_index_ = (schedule_index_ + 1) % schedule.size();
      if (!queue(candidate).empty())
        return candidate;
    }
    if (!direct_.empty())
      return Lane::direct;
    if (!explicit_.empty())
      return Lane::explicit_feature;
    return Lane::optional;
  }

  void run(const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
      PendingAiTask pending;
      {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this, stop_token] {
          return stop_token.stop_requested() || queued_unlocked() != 0;
        });
        if (stop_token.stop_requested())
          return;
        auto &selected = queue(next_lane());
        pending = std::move(selected.front());
        selected.pop_front();
        ++active_;
      }
      try {
        pending.task(stop_token);
      } catch (...) {
      }
      {
        const std::scoped_lock lock{mutex_};
        --active_;
      }
    }
  }

  const std::size_t capacity_;
  const std::size_t worker_count_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<PendingAiTask> direct_;
  std::deque<PendingAiTask> explicit_;
  std::deque<PendingAiTask> optional_;
  std::vector<std::jthread> workers_;
  std::size_t schedule_index_{};
  std::size_t active_{};
  bool started_{};
  bool accepting_{};
};

AiWorkService::AiWorkService(const std::size_t capacity,
                             const std::size_t worker_count)
    : impl_{std::make_unique<Impl>(capacity, worker_count)} {}

AiWorkService::~AiWorkService() { stop(); }

void AiWorkService::start() { impl_->start(); }

void AiWorkService::stop() noexcept { impl_->stop(); }

SubmitResult AiWorkService::submit(BoundedExecutor::Task task) {
  return impl_->submit(Lane::direct, std::move(task), {});
}

SubmitResult
AiWorkService::submit_explicit(BoundedExecutor::Task task,
                               BoundedExecutor::Cancellation cancellation) {
  return impl_->submit(Lane::explicit_feature, std::move(task),
                       std::move(cancellation));
}

SubmitResult
AiWorkService::submit_optional(BoundedExecutor::Task task,
                               BoundedExecutor::Cancellation cancellation) {
  return impl_->submit(Lane::optional, std::move(task),
                       std::move(cancellation));
}

SubmitResult
AiWorkService::submit_priority(BoundedExecutor::Task task,
                               BoundedExecutor::Cancellation cancellation) {
  return submit_explicit(std::move(task), std::move(cancellation));
}

QueueSnapshot AiWorkService::snapshot() const { return impl_->snapshot(); }

} // namespace sanguinius
