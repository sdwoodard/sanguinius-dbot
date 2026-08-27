#pragma once

#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <memory>

namespace sanguinius {

class AiWorkService {
public:
  explicit AiWorkService(std::size_t capacity = 64,
                         std::size_t worker_count = 2);
  ~AiWorkService();

  AiWorkService(const AiWorkService &) = delete;
  AiWorkService &operator=(const AiWorkService &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SubmitResult submit(BoundedExecutor::Task task);
  [[nodiscard]] SubmitResult
  submit_explicit(BoundedExecutor::Task task,
                  BoundedExecutor::Cancellation cancellation = {});
  [[nodiscard]] SubmitResult
  submit_optional(BoundedExecutor::Task task,
                  BoundedExecutor::Cancellation cancellation = {});
  [[nodiscard]] SubmitResult
  submit_priority(BoundedExecutor::Task task,
                  BoundedExecutor::Cancellation cancellation = {});
  [[nodiscard]] QueueSnapshot snapshot() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
