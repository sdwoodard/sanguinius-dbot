#pragma once

#include <string>
#include <string_view>

namespace sanguinius {

class ReliabilityTestService {
public:
  [[nodiscard]] std::string run(std::string_view scenario) const;
};

} // namespace sanguinius
