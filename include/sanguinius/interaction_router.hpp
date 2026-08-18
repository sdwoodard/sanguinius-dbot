#pragma once

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/interaction_handler.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <memory>

namespace sanguinius {

class InteractionRouter {
public:
  InteractionRouter(const ServerScopePolicy &scope_policy,
                    ControlConfiguration controls, InteractionHandler &handler,
                    Diagnostics &diagnostics);
  ~InteractionRouter();

  InteractionRouter(const InteractionRouter &) = delete;
  InteractionRouter &operator=(const InteractionRouter &) = delete;

  void route(IncomingInteraction interaction) const;
  void stop() noexcept;

private:
  class State;
  std::shared_ptr<State> state_;
};

} // namespace sanguinius
