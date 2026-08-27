#include "sanguinius/cross_feature_orchestrator.hpp"

#include <stdexcept>
#include <utility>

namespace sanguinius {

CrossFeatureOrchestrator::CrossFeatureOrchestrator(
    Diagnostics &diagnostics, const std::chrono::milliseconds recovery_interval)
    : diagnostics_{diagnostics}, recovery_interval_{recovery_interval} {
  if (recovery_interval_ <= std::chrono::milliseconds::zero())
    throw std::invalid_argument{
        "Cross-feature recovery interval must be positive."};
}

CrossFeatureOrchestrator::~CrossFeatureOrchestrator() { stop(); }

void CrossFeatureOrchestrator::add_consumer(std::string name,
                                            Consumer consumer) {
  if (name.empty() || !consumer)
    throw std::invalid_argument{"Cross-feature consumer is invalid."};
  const std::scoped_lock lock{mutex_};
  if (running_ || worker_.joinable())
    throw std::logic_error{
        "Cross-feature consumers must be registered before start."};
  consumers_.push_back(
      RegisteredConsumer{.name = std::move(name), .run = std::move(consumer)});
}

void CrossFeatureOrchestrator::start() {
  const std::scoped_lock lock{mutex_};
  if (running_ || worker_.joinable())
    throw std::logic_error{"Cross-feature orchestrator already started."};
  stopping_ = false;
  running_ = true;
  wake_requested_ = true;
  worker_ = std::thread{[this] { loop(); }};
}

void CrossFeatureOrchestrator::stop() noexcept {
  {
    const std::scoped_lock lock{mutex_};
    if (!worker_.joinable()) {
      running_ = false;
      return;
    }
    stopping_ = true;
    wake_requested_ = true;
  }
  wakeup_.notify_all();
  worker_.join();
  const std::scoped_lock lock{mutex_};
  running_ = false;
  stopping_ = false;
  wake_requested_ = false;
}

void CrossFeatureOrchestrator::wake() noexcept {
  {
    const std::scoped_lock lock{mutex_};
    wake_requested_ = true;
  }
  wakeup_.notify_one();
}

CrossFeatureHealth CrossFeatureOrchestrator::health() const {
  const std::scoped_lock lock{mutex_};
  CrossFeatureHealth result{
      .running = running_,
      .consumer_count = consumers_.size(),
      .completed_passes = completed_passes_,
      .failed_consumers = failed_consumers_,
      .consumers = {},
  };
  result.consumers.reserve(consumers_.size());
  for (const auto &consumer : consumers_) {
    result.consumers.push_back({.name = consumer.name,
                                .degraded = consumer.degraded,
                                .backlog = consumer.backlog,
                                .failures = consumer.failures});
  }
  return result;
}

void CrossFeatureOrchestrator::loop() {
  std::unique_lock lock{mutex_};
  while (!stopping_) {
    wakeup_.wait_for(lock, recovery_interval_,
                     [this] { return stopping_ || wake_requested_; });
    if (stopping_)
      break;
    wake_requested_ = false;
    lock.unlock();
    const bool more_work = run_pass();
    lock.lock();
    ++completed_passes_;
    if (more_work)
      wake_requested_ = true;
  }
}

bool CrossFeatureOrchestrator::run_pass() noexcept {
  bool more_work = false;
  for (std::size_t index = 0; index < consumers_.size(); ++index) {
    const auto &consumer = consumers_[index];
    try {
      const bool backlog = consumer.run();
      {
        const std::scoped_lock lock{mutex_};
        consumers_[index].degraded = false;
        consumers_[index].backlog = backlog;
      }
      more_work = backlog || more_work;
    } catch (const std::exception &) {
      {
        const std::scoped_lock lock{mutex_};
        ++failed_consumers_;
        consumers_[index].degraded = true;
        consumers_[index].backlog = true;
        ++consumers_[index].failures;
      }
      diagnostics_.emit(
          {DiagnosticSeverity::error,
           "cross_feature." + consumer.name,
           "Cross-feature consumer failed; durable work remains pending.",
           {}});
    } catch (...) {
      {
        const std::scoped_lock lock{mutex_};
        ++failed_consumers_;
        consumers_[index].degraded = true;
        consumers_[index].backlog = true;
        ++consumers_[index].failures;
      }
      diagnostics_.emit({DiagnosticSeverity::error,
                         "cross_feature." + consumer.name,
                         "Unknown cross-feature consumer failure; durable work "
                         "remains pending.",
                         {}});
    }
  }
  return more_work;
}

MessageObservationPipeline::MessageObservationPipeline(Diagnostics &diagnostics)
    : diagnostics_{diagnostics} {}

void MessageObservationPipeline::add_observer(std::string name,
                                              Observer observer) {
  if (name.empty() || !observer)
    throw std::invalid_argument{"Message observer is invalid."};
  observers_.push_back(RegisteredObserver{.name = std::move(name),
                                          .observe = std::move(observer)});
}

void MessageObservationPipeline::observe(
    const IncomingMessage &message) const noexcept {
  for (const auto &observer : observers_) {
    try {
      observer.observe(message);
    } catch (const std::exception &) {
      diagnostics_.emit(
          {DiagnosticSeverity::error, "message_observer." + observer.name,
           "Message observation failed; the remaining observers will continue.",
           message.correlation_id});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error,
                         "message_observer." + observer.name,
                         "Unknown message observation failure; the remaining "
                         "observers will continue.",
                         message.correlation_id});
    }
  }
}

} // namespace sanguinius
