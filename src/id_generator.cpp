#include "sanguinius/id_generator.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace sanguinius {

ProcessIdGenerator::ProcessIdGenerator() {
  std::random_device random;
  prefix_ = (static_cast<std::uint64_t>(random()) << 32U) |
            static_cast<std::uint64_t>(random());
}

std::string ProcessIdGenerator::next_id() {
  const auto counter = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << prefix_ << '-'
         << std::setw(16) << counter;
  return stream.str();
}

} // namespace sanguinius
