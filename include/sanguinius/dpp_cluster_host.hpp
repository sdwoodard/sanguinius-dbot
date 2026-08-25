#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace dpp {
class cluster;
}

namespace sanguinius {

class DppClusterHost final {
public:
  DppClusterHost(std::string token, bool voice_enabled);
  ~DppClusterHost();

  DppClusterHost(const DppClusterHost &) = delete;
  DppClusterHost &operator=(const DppClusterHost &) = delete;

  [[nodiscard]] dpp::cluster &native() noexcept;
  [[nodiscard]] std::uint32_t intents() const noexcept;
  void start();
  void shutdown() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
