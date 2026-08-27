#pragma once

#include "sanguinius/provider_circuit.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRepositoryContext;

class SqliteProviderCircuitRepository final : public ProviderCircuitRepository {
public:
  explicit SqliteProviderCircuitRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void restart(std::string_view provider, std::int64_t now_ms,
               std::string transition_id) override;
  [[nodiscard]] bool admit(std::string_view provider, std::int64_t now_ms,
                           std::string transition_id) override;
  [[nodiscard]] std::string state(std::string_view provider) const override;
  void succeeded(std::string_view provider, std::int64_t now_ms,
                 std::string transition_id) override;
  void failed(std::string_view provider, ProviderCircuitFailure failure,
              std::string_view reason_code, std::int64_t now_ms,
              std::string transition_id) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
