#pragma once

#include <string>

namespace sanguinius {

class PersistentIdGenerator {
public:
  virtual ~PersistentIdGenerator() = default;
  [[nodiscard]] virtual std::string next_id() = 0;
};

class UuidV4Generator final : public PersistentIdGenerator {
public:
  [[nodiscard]] std::string next_id() override;
};

[[nodiscard]] bool valid_uuid_v4(const std::string &value) noexcept;

} // namespace sanguinius
