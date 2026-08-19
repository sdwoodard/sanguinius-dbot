#include "sanguinius/ai_work_service.hpp"

namespace sanguinius {

AiWorkService::AiWorkService(const std::size_t capacity,
                             const std::size_t worker_count)
    : workers_{capacity, worker_count} {}

AiWorkService::~AiWorkService() { stop(); }

void AiWorkService::start() { workers_.start(); }

void AiWorkService::stop() noexcept { workers_.stop(); }

SubmitResult AiWorkService::submit(BoundedExecutor::Task task) {
  return workers_.try_submit(std::move(task));
}

SubmitResult AiWorkService::submit_priority(
    BoundedExecutor::Task task, BoundedExecutor::Cancellation cancellation) {
  return workers_.try_submit_front(std::move(task), std::move(cancellation));
}

QueueSnapshot AiWorkService::snapshot() const { return workers_.snapshot(); }

} // namespace sanguinius
