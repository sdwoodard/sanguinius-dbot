#pragma once

#include "sanguinius/repositories.hpp"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace sanguinius::test {

struct RecordedStop {
  std::string instance_id;
  std::int64_t stopped_at_ms{};
  ApplicationStopReason reason{ApplicationStopReason::clean_shutdown};
};

class FakeApplicationInstanceRepository final
    : public ApplicationInstanceRepository {
public:
  void record_start(const ApplicationInstanceRecord &record) override {
    std::scoped_lock lock{mutex_};
    if (fail_start_) {
      throw std::runtime_error{"scripted instance start failure"};
    }
    starts_.push_back(record);
  }

  void record_stop(const std::string &instance_id,
                   const std::int64_t stopped_at_ms,
                   const ApplicationStopReason reason) override {
    std::scoped_lock lock{mutex_};
    if (fail_stop_) {
      throw std::runtime_error{"scripted instance stop failure"};
    }
    stops_.push_back({instance_id, stopped_at_ms, reason});
  }

  [[nodiscard]] std::vector<ApplicationInstanceRecord> starts() const {
    std::scoped_lock lock{mutex_};
    return starts_;
  }

  [[nodiscard]] std::vector<RecordedStop> stops() const {
    std::scoped_lock lock{mutex_};
    return stops_;
  }

  void fail_start(bool value = true) { fail_start_ = value; }
  void fail_stop(bool value = true) { fail_stop_ = value; }

private:
  mutable std::mutex mutex_;
  std::vector<ApplicationInstanceRecord> starts_;
  std::vector<RecordedStop> stops_;
  bool fail_start_{false};
  bool fail_stop_{false};
};

} // namespace sanguinius::test
