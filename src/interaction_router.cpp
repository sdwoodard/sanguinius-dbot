#include "sanguinius/interaction_router.hpp"

#include "sanguinius/pending_notice.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace sanguinius {
namespace {

void reply_ephemeral(const IncomingInteraction &interaction,
                     const std::string &content) {
  if (interaction.responder) {
    interaction.responder->reply(text_message(content),
                                 ResponseVisibility::ephemeral);
  }
}

[[nodiscard]] std::optional<InteractionOperation>
slash_operation(const IncomingInteraction &interaction) {
  if (interaction.command_name == "sanguinius") {
    if (interaction.subcommand_name == "status") {
      return InteractionOperation::status;
    }
    if (interaction.subcommand_name == "inbox") {
      return InteractionOperation::inbox;
    }
    if (interaction.subcommand_name == "privacy") {
      return InteractionOperation::privacy;
    }
  }
  if (interaction.command_name == "sang-admin") {
    if (interaction.subcommand_name == "health") {
      return InteractionOperation::admin_health;
    }
    if (interaction.subcommand_name == "test-notice") {
      return InteractionOperation::test_notice;
    }
  }
  return std::nullopt;
}

} // namespace

class InteractionRouter::State {
public:
  State(const ServerScopePolicy &scope_policy_value,
        const ControlConfiguration controls_value,
        InteractionHandler &handler_value, Diagnostics &diagnostics_value)
      : scope_policy{scope_policy_value}, controls{controls_value},
        handler{handler_value}, diagnostics{diagnostics_value} {}

  const ServerScopePolicy &scope_policy;
  ControlConfiguration controls;
  InteractionHandler &handler;
  Diagnostics &diagnostics;
  std::mutex mutex;
  bool accepting{true};
};

InteractionRouter::InteractionRouter(const ServerScopePolicy &scope_policy,
                                     const ControlConfiguration controls,
                                     InteractionHandler &handler,
                                     Diagnostics &diagnostics)
    : state_{std::make_shared<State>(scope_policy, controls, handler,
                                     diagnostics)} {}

InteractionRouter::~InteractionRouter() { stop(); }

void InteractionRouter::route(IncomingInteraction interaction) const {
  if (!interaction.responder || !interaction.interaction_id.is_set() ||
      !interaction.user_id.is_set()) {
    return;
  }
  const auto member_scope = state_->scope_policy.authorize(
      {interaction.guild_id, interaction.channel_id, interaction.user_id},
      RequiredRole::member);
  if (!member_scope.allowed()) {
    reply_ephemeral(interaction,
                    "Use Sanguinius interactions in the configured primary "
                    "guild channel.");
    return;
  }

  std::optional<InteractionOperation> operation;
  if (interaction.kind == InteractionKind::slash_command) {
    if (!interaction.command_options.empty() ||
        !interaction.custom_id.empty() ||
        !interaction.selected_values.empty() ||
        !interaction.modal_fields.empty() || interaction.context_message) {
      reply_ephemeral(interaction, "This command request is malformed.");
      return;
    }
    operation = slash_operation(interaction);
    if (!operation.has_value()) {
      reply_ephemeral(interaction, "That Sanguinius command is not available.");
      return;
    }
  } else if (interaction.kind == InteractionKind::button ||
             interaction.kind == InteractionKind::select_menu ||
             interaction.kind == InteractionKind::modal_submit) {
    const bool malformed_button = interaction.kind == InteractionKind::button &&
                                  (!interaction.selected_values.empty() ||
                                   !interaction.modal_fields.empty());
    const bool malformed_select =
        interaction.kind == InteractionKind::select_menu &&
        !interaction.modal_fields.empty();
    const bool malformed_modal =
        interaction.kind == InteractionKind::modal_submit &&
        !interaction.selected_values.empty();
    if (!interaction.command_name.empty() ||
        !interaction.subcommand_name.empty() ||
        !interaction.command_options.empty() || interaction.context_message ||
        malformed_button || malformed_select || malformed_modal) {
      reply_ephemeral(interaction, "This control request is malformed.");
      return;
    }
    if (!parse_component_token(interaction.custom_id).has_value()) {
      reply_ephemeral(interaction,
                      "This control is invalid or no longer available.");
      return;
    }
    operation = InteractionOperation::open_component;
  } else {
    reply_ephemeral(interaction, "That context action is not available yet.");
    return;
  }

  if (*operation == InteractionOperation::admin_health ||
      *operation == InteractionOperation::test_notice) {
    if (!state_->controls.admin_commands_enabled) {
      state_->diagnostics.emit(
          {DiagnosticSeverity::warning, "interaction.admin_rejected",
           "Owner interaction was rejected because administration is disabled.",
           interaction.correlation_id});
      reply_ephemeral(interaction,
                      "Owner administration is currently disabled.");
      return;
    }
    const auto owner_scope = state_->scope_policy.authorize(
        {interaction.guild_id, interaction.channel_id, interaction.user_id},
        RequiredRole::owner);
    if (!owner_scope.allowed()) {
      state_->diagnostics.emit(
          {DiagnosticSeverity::warning, "interaction.admin_rejected",
           "Owner interaction was rejected by authorization policy.",
           interaction.correlation_id});
      reply_ephemeral(interaction, "This owner-only command is unavailable.");
      return;
    }
    if (*operation == InteractionOperation::test_notice &&
        !state_->controls.test_mode) {
      state_->diagnostics.emit(
          {DiagnosticSeverity::warning, "interaction.admin_rejected",
           "Owner test interaction was rejected because test mode is disabled.",
           interaction.correlation_id});
      reply_ephemeral(interaction, "Owner test mode is currently disabled.");
      return;
    }
  }

  auto shared_state = state_;
  auto queued = RoutedInteraction{std::move(interaction), *operation};
  queued.interaction.responder->defer(
      ResponseVisibility::ephemeral, [shared_state, queued = std::move(queued)](
                                         const DeliveryResult result) mutable {
        if (result != DeliveryResult::success) {
          shared_state->diagnostics.emit(
              {DiagnosticSeverity::warning, "interaction.defer",
               "Discord did not confirm the interaction acknowledgement.",
               queued.interaction.correlation_id});
          return;
        }
        const std::scoped_lock lock{shared_state->mutex};
        if (shared_state->accepting) {
          static_cast<void>(shared_state->handler.enqueue(std::move(queued)));
        }
      });
}

void InteractionRouter::stop() noexcept {
  try {
    const std::scoped_lock lock{state_->mutex};
    state_->accepting = false;
  } catch (...) {
  }
}

} // namespace sanguinius
