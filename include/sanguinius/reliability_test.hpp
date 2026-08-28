#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace sanguinius {

class ReliabilityTestService {
public:
  virtual ~ReliabilityTestService() = default;
  [[nodiscard]] virtual std::string run(std::string_view scenario) const = 0;
};

[[nodiscard]] std::unique_ptr<ReliabilityTestService>
make_isolated_reliability_test_service();

} // namespace sanguinius
