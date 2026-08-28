#include "sanguinius/reliability_test.hpp"

#include "sanguinius/ai_generation.hpp"
#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/outbox.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace sanguinius {

std::string ReliabilityTestService::run(const std::string_view scenario) const {
  if (scenario == "text-timeout") {
    const auto unsent =
        classify_ai_provider_failure(AiProviderErrorCategory::timeout, false);
    const auto submitted =
        classify_ai_provider_failure(AiProviderErrorCategory::timeout, true);
    if (unsent.action != AiFailureAccounting::cancel_reservation ||
        unsent.result_code != "provider_not_sent" ||
        submitted.action != AiFailureAccounting::fail_attempt ||
        submitted.result_code != "timeout")
      throw std::runtime_error{"Text timeout accounting probe failed."};
    return "Reliability probe passed: an unsent timeout releases its "
           "reservation; a submitted timeout is accounted as failed. No "
           "provider request was made.";
  }
  if (scenario == "ai-saturation") {
    AiWorkService work{1, 1};
    std::mutex mutex;
    std::condition_variable_any changed;
    bool active{};
    bool release{};
    work.start();
    const auto first = work.submit([&](const std::stop_token stop_token) {
      std::unique_lock lock{mutex};
      active = true;
      changed.notify_all();
      changed.wait_for(lock, stop_token, std::chrono::milliseconds{500},
                       [&] { return release; });
    });
    {
      std::unique_lock lock{mutex};
      if (!changed.wait_for(lock, std::chrono::milliseconds{500},
                            [&] { return active; })) {
        work.stop();
        throw std::runtime_error{"AI saturation probe did not start."};
      }
    }
    const auto queued = work.submit([](std::stop_token) {});
    const auto rejected = work.submit([](std::stop_token) {});
    {
      const std::scoped_lock lock{mutex};
      release = true;
    }
    changed.notify_all();
    work.stop();
    if (first != SubmitResult::accepted || queued != SubmitResult::accepted ||
        rejected != SubmitResult::full)
      throw std::runtime_error{"AI saturation admission probe failed."};
    return "Reliability probe passed: the bounded AI queue rejected excess "
           "work without provider traffic.";
  }
  if (scenario == "discord-unknown") {
    const auto retry = classify_discord_delivery_failure(
        DeliveryResult::unknown_outcome, true);
    const auto quarantined = classify_discord_delivery_failure(
        DeliveryResult::unknown_outcome, false);
    if (!retry.retry || retry.mode != OutboxFailureMode::retryable ||
        retry.error_code != "discord_unknown_outcome" || quarantined.retry ||
        quarantined.mode != OutboxFailureMode::failed ||
        quarantined.error_code != "discord_unknown_outcome_stale")
      throw std::runtime_error{"Discord unknown-outcome probe failed."};
    return "Reliability probe passed: an unknown Discord outcome retries only "
           "inside the nonce window and is quarantined when stale. No public "
           "message was submitted.";
  }
  throw std::invalid_argument{"Unknown reliability scenario."};
}

} // namespace sanguinius
