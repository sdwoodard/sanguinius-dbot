#include "sanguinius/interaction_router.hpp"

#include "sanguinius/chronicle.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/pending_notice.hpp"

#include <algorithm>
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
    if (interaction.subcommand_name == "work-recent") {
      return InteractionOperation::work_recent;
    }
    if (interaction.subcommand_name == "work-dead") {
      return InteractionOperation::work_dead;
    }
    if (interaction.subcommand_name == "test-notice") {
      return InteractionOperation::test_notice;
    }
    if (interaction.subcommand_name == "test-schedule-notice") {
      return InteractionOperation::test_schedule_notice;
    }
    if (interaction.subcommand_name == "test-public-retry") {
      return InteractionOperation::test_public_retry;
    }
    if (interaction.subcommand_name == "test-anniversary")
      return InteractionOperation::test_anniversary;
  }
  if (interaction.command_name == "chronicle") {
    if (interaction.subcommand_group_name == "session") {
      if (interaction.subcommand_name == "start") return InteractionOperation::chronicle_session_start;
      if (interaction.subcommand_name == "status") return InteractionOperation::chronicle_session_status;
      if (interaction.subcommand_name == "close") return InteractionOperation::chronicle_session_close;
      if (interaction.subcommand_name == "edit") return InteractionOperation::chronicle_summary_edit;
      if (interaction.subcommand_name == "approve") return InteractionOperation::chronicle_summary_approve;
      if (interaction.subcommand_name == "reject") return InteractionOperation::chronicle_summary_reject;
    }
    if (interaction.subcommand_group_name == "title") {
      if (interaction.subcommand_name == "propose") return InteractionOperation::chronicle_title_propose;
      if (interaction.subcommand_name == "list") return InteractionOperation::chronicle_title_list;
      if (interaction.subcommand_name == "approve") return InteractionOperation::chronicle_title_approve;
      if (interaction.subcommand_name == "reject") return InteractionOperation::chronicle_title_reject;
      if (interaction.subcommand_name == "feature") return InteractionOperation::chronicle_title_feature;
      if (interaction.subcommand_name == "revoke") return InteractionOperation::chronicle_title_revoke;
    }
    if (interaction.subcommand_group_name == "anniversaries") {
      if (interaction.subcommand_name == "on") return InteractionOperation::chronicle_anniversaries_on;
      if (interaction.subcommand_name == "off") return InteractionOperation::chronicle_anniversaries_off;
    }
    if (interaction.subcommand_name == "recall") return InteractionOperation::chronicle_recall;
    if (interaction.subcommand_name == "timeline") return InteractionOperation::chronicle_timeline;
    if (interaction.subcommand_name == "forget") return InteractionOperation::chronicle_forget;
    if (interaction.subcommand_name == "profile") return InteractionOperation::chronicle_profile;
    if (interaction.subcommand_name == "callbacks") return InteractionOperation::chronicle_callbacks;
  }
  return std::nullopt;
}

[[nodiscard]] bool valid_slash_shape(const IncomingInteraction &interaction) {
  const auto catalog = command_catalog(true, true);
  const auto command = std::ranges::find(catalog.commands,
                                         interaction.command_name,
                                         &CommandDefinition::name);
  if (command == catalog.commands.end() ||
      command->kind != ApplicationCommandKind::chat_input)
    return false;
  const CommandSubcommandDefinition *definition = nullptr;
  if (interaction.subcommand_group_name.empty()) {
    const auto found = std::ranges::find(command->subcommands,
                                         interaction.subcommand_name,
                                         &CommandSubcommandDefinition::name);
    if (found != command->subcommands.end()) definition = &*found;
  } else {
    const auto group = std::ranges::find(command->subcommand_groups,
                                         interaction.subcommand_group_name,
                                         &CommandSubcommandGroupDefinition::name);
    if (group == command->subcommand_groups.end()) return false;
    const auto found = std::ranges::find(group->subcommands,
                                         interaction.subcommand_name,
                                         &CommandSubcommandDefinition::name);
    if (found != group->subcommands.end()) definition = &*found;
  }
  if (definition == nullptr ||
      interaction.command_options.size() > definition->options.size())
    return false;
  for (const auto &expected : definition->options) {
    const auto count = std::count_if(
        interaction.command_options.begin(), interaction.command_options.end(),
        [&expected](const InteractionOption &option) {
          return option.name == expected.name;
        });
    if ((expected.required && count != 1) || (!expected.required && count > 1))
      return false;
    if (count == 0) continue;
    const auto found = std::ranges::find(interaction.command_options,
                                         expected.name,
                                         &InteractionOption::name);
    if (expected.kind == CommandOptionKind::user) {
      const auto *value = std::get_if<DiscordId>(&found->value);
      if (value == nullptr || !value->is_set()) return false;
    } else {
      const auto *value = std::get_if<std::string>(&found->value);
      if (value == nullptr || value->size() < expected.minimum_length ||
          value->size() > expected.maximum_length)
        return false;
      if (!expected.choices.empty() &&
          std::ranges::none_of(expected.choices, [value](const auto &choice) {
            return choice.value == *value;
          }))
        return false;
    }
  }
  return std::all_of(interaction.command_options.begin(),
                     interaction.command_options.end(),
                     [definition](const InteractionOption &option) {
                       return std::ranges::any_of(
                           definition->options, [&option](const auto &expected) {
                             return expected.name == option.name;
                           });
                     });
}

} // namespace

class InteractionRouter::State {
public:
  State(const ServerScopePolicy &scope_policy_value,
        const ControlConfiguration controls_value,
        const FeatureConfiguration features_value,
        InteractionHandler &handler_value, Diagnostics &diagnostics_value)
      : scope_policy{scope_policy_value}, controls{controls_value},
        features{features_value}, handler{handler_value}, diagnostics{diagnostics_value} {}

  const ServerScopePolicy &scope_policy;
  ControlConfiguration controls;
  FeatureConfiguration features;
  InteractionHandler &handler;
  Diagnostics &diagnostics;
  std::mutex mutex;
  bool accepting{true};
};

InteractionRouter::InteractionRouter(const ServerScopePolicy &scope_policy,
                                     const ControlConfiguration controls,
                                     const FeatureConfiguration features,
                                     InteractionHandler &handler,
                                     Diagnostics &diagnostics)
    : state_{std::make_shared<State>(scope_policy, controls, features, handler,
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
    if (!interaction.custom_id.empty() ||
        !interaction.selected_values.empty() ||
        !interaction.modal_fields.empty() || interaction.context_message) {
      reply_ephemeral(interaction, "This command request is malformed.");
      return;
    }
    if (!valid_slash_shape(interaction)) {
      reply_ephemeral(interaction, "This Chronicle command is malformed.");
      return;
    }
    if (interaction.command_name == "chronicle" &&
        interaction.subcommand_name == "remember") {
      if (!state_->features.chronicle_enabled ||
          !interaction.command_options.empty()) {
        reply_ephemeral(interaction, "The Chronicle is currently unavailable.");
        return;
      }
      interaction.responder->show_modal(ChronicleService::remember_modal());
      return;
    }
    operation = slash_operation(interaction);
    if (!operation.has_value()) {
      reply_ephemeral(interaction, "That Sanguinius command is not available.");
      return;
    }
  } else if (interaction.kind == InteractionKind::message_context_command) {
    if (!state_->features.chronicle_enabled ||
        interaction.command_name != "Canonize in the Chronicle" ||
        !interaction.subcommand_name.empty() || !interaction.command_options.empty() ||
        !interaction.custom_id.empty() || !interaction.context_message.has_value()) {
      reply_ephemeral(interaction, "That context action is not available yet.");
      return;
    }
    operation = InteractionOperation::chronicle_canonize;
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
        !interaction.subcommand_group_name.empty() ||
        !interaction.subcommand_name.empty() ||
        !interaction.command_options.empty() || interaction.context_message ||
        malformed_button || malformed_select || malformed_modal) {
      reply_ephemeral(interaction, "This control request is malformed.");
      return;
    }
    if (interaction.kind == InteractionKind::modal_submit &&
        interaction.custom_id == "chronicle.remember:1") {
      if (!state_->features.chronicle_enabled) {
        reply_ephemeral(interaction, "The Chronicle is currently unavailable.");
        return;
      }
      operation = InteractionOperation::chronicle_memory_preview;
    } else if (interaction.kind == InteractionKind::button) {
      if (const auto edit_token = parse_chronicle_component(
              interaction.custom_id, chronicle_session_edit_prefix)) {
        if (!state_->features.chronicle_enabled) {
          reply_ephemeral(interaction, "The Chronicle is currently unavailable.");
          return;
        }
        interaction.responder->show_modal(
            ChronicleSessionService::summary_edit_modal(*edit_token));
        return;
      }
      if (const auto edit_token = parse_chronicle_component(
              interaction.custom_id, chronicle_modal_prefix)) {
        if (!state_->features.chronicle_enabled) {
          reply_ephemeral(interaction, "The Chronicle is currently unavailable.");
          return;
        }
        interaction.responder->show_modal(
            ChronicleService::edit_entry_modal(*edit_token));
        return;
      }
      if (parse_component_token(interaction.custom_id)) {
        operation = InteractionOperation::open_component;
      } else if (parse_chronicle_component(
                     interaction.custom_id,
                     chronicle_search_component_prefix)) {
        operation = InteractionOperation::chronicle_search_component;
      } else if (parse_chronicle_component(
                     interaction.custom_id,
                     chronicle_session_component_prefix)) {
        operation = InteractionOperation::chronicle_summary_component;
      } else if (parse_chronicle_component(interaction.custom_id,
                                           chronicle_component_prefix) ||
                 parse_chronicle_component(interaction.custom_id,
                                           memory_draft_component_prefix)) {
        if (!state_->features.chronicle_enabled) {
          reply_ephemeral(interaction, "The Chronicle is currently unavailable.");
          return;
        }
        operation = InteractionOperation::chronicle_component;
      }
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_chronicle_component(interaction.custom_id,
                                         chronicle_session_edit_prefix)) {
      operation = InteractionOperation::chronicle_summary_component;
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_chronicle_component(interaction.custom_id,
                                         chronicle_modal_prefix)) {
      operation = InteractionOperation::chronicle_edit;
    }
    if (!operation.has_value()) {
      reply_ephemeral(interaction,
                      "This control is invalid or no longer available.");
      return;
    }
  } else {
    reply_ephemeral(interaction, "That context action is not available yet.");
    return;
  }

  const bool chronicle_operation =
      *operation == InteractionOperation::chronicle_canonize ||
      *operation == InteractionOperation::chronicle_memory_preview ||
      *operation == InteractionOperation::chronicle_recall ||
      *operation == InteractionOperation::chronicle_timeline ||
      *operation == InteractionOperation::chronicle_forget ||
      *operation == InteractionOperation::chronicle_profile ||
      *operation == InteractionOperation::chronicle_callbacks ||
      *operation == InteractionOperation::chronicle_edit ||
      *operation == InteractionOperation::chronicle_component ||
      *operation == InteractionOperation::chronicle_summary_component ||
      *operation == InteractionOperation::chronicle_search_component;
  const bool chronicle_session_operation =
      *operation >= InteractionOperation::chronicle_session_start &&
      *operation <= InteractionOperation::chronicle_anniversaries_off;
  const bool anniversary_test =
      *operation == InteractionOperation::test_anniversary;
  if ((chronicle_operation || chronicle_session_operation || anniversary_test) &&
      !state_->features.chronicle_enabled) {
    reply_ephemeral(interaction, "The Chronicle is currently unavailable.");
    return;
  }

  const bool admin_operation =
      *operation == InteractionOperation::admin_health ||
      *operation == InteractionOperation::work_recent ||
      *operation == InteractionOperation::work_dead ||
      *operation == InteractionOperation::test_notice ||
      *operation == InteractionOperation::test_schedule_notice ||
      *operation == InteractionOperation::test_public_retry;
  const bool test_operation =
      *operation == InteractionOperation::test_notice ||
      *operation == InteractionOperation::test_schedule_notice ||
      *operation == InteractionOperation::test_public_retry;
  if (admin_operation || anniversary_test) {
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
    if ((test_operation || anniversary_test) && !state_->controls.test_mode) {
      state_->diagnostics.emit(
          {DiagnosticSeverity::warning, "interaction.admin_rejected",
           "Owner test interaction was rejected because test mode is disabled.",
           interaction.correlation_id});
      reply_ephemeral(interaction, "Owner test mode is currently disabled.");
      return;
    }
  }

  auto shared_state = state_;
  bool public_profile = false;
  if (*operation == InteractionOperation::chronicle_profile &&
      !interaction.command_options.empty()) {
    if (const auto *target =
            std::get_if<DiscordId>(&interaction.command_options.front().value)) {
      public_profile = *target != interaction.user_id;
    }
  }
  auto queued = RoutedInteraction{std::move(interaction), *operation};
  queued.interaction.responder->defer(
      (*operation == InteractionOperation::chronicle_timeline || public_profile)
          ? ResponseVisibility::public_message
          : ResponseVisibility::ephemeral,
      [shared_state, queued = std::move(queued)](
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
