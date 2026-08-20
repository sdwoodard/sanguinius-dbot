#include "sanguinius/random.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <sys/random.h>

namespace sanguinius {

std::uint64_t SystemRandom::uniform(const std::uint64_t upper_exclusive) {
  if (upper_exclusive == 0) {
    throw std::invalid_argument{"Random upper bound must be positive."};
  }

  const auto rejection_limit =
      std::numeric_limits<std::uint64_t>::max() -
      (std::numeric_limits<std::uint64_t>::max() % upper_exclusive);
  for (;;) {
    std::uint64_t sample{};
    std::size_t offset{};
    auto *bytes = reinterpret_cast<unsigned char *>(&sample);
    while (offset < sizeof(sample)) {
      const auto received =
          ::getrandom(bytes + offset, sizeof(sample) - offset, 0);
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error{errno, std::generic_category(),
                                "Unable to obtain random bytes"};
      }
      if (received == 0) {
        throw std::runtime_error{"Unable to obtain random bytes."};
      }
      offset += static_cast<std::size_t>(received);
    }
    if (sample < rejection_limit) {
      return sample % upper_exclusive;
    }
  }
}

} // namespace sanguinius
