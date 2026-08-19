#pragma once

#include "sanguinius/work_queue.hpp"

#include <cstddef>

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
  submit_priority(BoundedExecutor::Task task,
                  BoundedExecutor::Cancellation cancellation = {});
  [[nodiscard]] QueueSnapshot snapshot() const;

private:
  BoundedExecutor workers_;
};

} // namespace sanguinius
