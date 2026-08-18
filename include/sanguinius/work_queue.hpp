#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>

namespace sanguinius {

enum class SubmitResult {
  accepted,
  full,
  stopping,
};

struct QueueSnapshot {
  std::size_t capacity{};
  std::size_t queued{};
  std::size_t active{};
  bool accepting{};
};

class BoundedExecutor {
public:
  using Task = std::function<void(std::stop_token)>;

  BoundedExecutor(std::size_t capacity, std::size_t worker_count);
  ~BoundedExecutor();

  BoundedExecutor(const BoundedExecutor &) = delete;
  BoundedExecutor &operator=(const BoundedExecutor &) = delete;
  BoundedExecutor(BoundedExecutor &&) = delete;
  BoundedExecutor &operator=(BoundedExecutor &&) = delete;

  void start();
  [[nodiscard]] SubmitResult try_submit(Task task);
  [[nodiscard]] QueueSnapshot snapshot() const;
  void stop() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
