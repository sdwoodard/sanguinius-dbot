#pragma once

#include <atomic>
#include <string_view>

namespace sanguinius {

class ServiceNotifier {
public:
  virtual ~ServiceNotifier() = default;

  virtual void status(std::string_view message) noexcept = 0;
  virtual void ready(std::string_view message) noexcept = 0;
  virtual void watchdog(std::string_view message) noexcept = 0;
  virtual void stopping() noexcept = 0;
};

class SystemdServiceNotifier final : public ServiceNotifier {
public:
  void status(std::string_view message) noexcept override;
  void ready(std::string_view message) noexcept override;
  void watchdog(std::string_view message) noexcept override;
  void stopping() noexcept override;

private:
  std::atomic<bool> ready_sent_{false};
};

class NoopServiceNotifier final : public ServiceNotifier {
public:
  void status(std::string_view) noexcept override {}
  void ready(std::string_view) noexcept override {}
  void watchdog(std::string_view) noexcept override {}
  void stopping() noexcept override {}
};

} // namespace sanguinius
