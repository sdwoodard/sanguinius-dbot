#include "sanguinius/interaction_router.hpp"

#include "sanguinius/chronicle.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/pending_notice.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/presentation.hpp"
#include "sanguinius/tarot.hpp"
#include "sanguinius/tarot_house.hpp"
#include "sanguinius/wagers.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace sanguinius {
namespace {

void reply_ephemeral(const IncomingInteraction &interaction,
                     const presentation::ErrorKind kind,
                     const std::string_view retry_command = {}) {
  if (interaction.responder) {
    interaction.responder->reply(presentation::error(kind, retry_command),
                                 ResponseVisibility::ephemeral);
  }
}

[[nodiscard]] std::optional<InteractionOperation>
slash_operation(const IncomingInteraction &interaction) {
  if (interaction.command_name == "help")
    return InteractionOperation::help;
  if (interaction.command_name == "repo")
    return InteractionOperation::repo;
  if (interaction.command_name == "sanguinius") {
    if (interaction.subcommand_group_name == "quiet") {
      if (interaction.subcommand_name == "for")
        return InteractionOperation::appearance_quiet_for;
      if (interaction.subcommand_name == "tonight")
        return InteractionOperation::appearance_quiet_tonight;
      if (interaction.subcommand_name == "until")
        return InteractionOperation::appearance_quiet_until;
      if (interaction.subcommand_name == "off")
        return InteractionOperation::appearance_quiet_off;
    }
    if (interaction.subcommand_name == "status") {
      return InteractionOperation::status;
    }
    if (interaction.subcommand_name == "inbox") {
      return InteractionOperation::inbox;
    }
    if (interaction.subcommand_name == "privacy") {
      return InteractionOperation::privacy;
    }
    if (interaction.subcommand_name == "appearance-callbacks") {
      return InteractionOperation::appearance_callbacks;
    }
    if (interaction.subcommand_name == "appearance-feedback")
      return InteractionOperation::appearance_feedback;
  }
  if (interaction.command_name == "sang-admin") {
    if (interaction.subcommand_group_name == "reliability-test" &&
        (interaction.subcommand_name == "text-timeout" ||
         interaction.subcommand_name == "ai-saturation" ||
         interaction.subcommand_name == "discord-unknown"))
      return InteractionOperation::reliability_test;
    if (interaction.subcommand_group_name == "safety" &&
        interaction.subcommand_name == "status")
      return InteractionOperation::safety_status;
    if (interaction.subcommand_group_name == "safety" &&
        interaction.subcommand_name == "set") {
      const auto target = std::ranges::find(interaction.command_options,
                                            "target", &InteractionOption::name);
      const auto mode = std::ranges::find(interaction.command_options, "mode",
                                          &InteractionOption::name);
      const auto *target_value = target == interaction.command_options.end()
                                     ? nullptr
                                     : std::get_if<std::string>(&target->value);
      const auto *mode_value = mode == interaction.command_options.end()
                                   ? nullptr
                                   : std::get_if<std::string>(&mode->value);
      if (target_value == nullptr || mode_value == nullptr)
        return std::nullopt;
      const bool disable = *mode_value == "disabled";
      if (*mode_value != "enabled" && !disable)
        return std::nullopt;
      if (*target_value == "appearances")
        return disable ? InteractionOperation::appearance_disable
                       : InteractionOperation::appearance_enable;
      if (*target_value == "voice-input")
        return disable ? InteractionOperation::vox_listening_disable
                       : InteractionOperation::vox_listening_enable;
      return InteractionOperation::safety_set;
    }
    if (interaction.subcommand_group_name == "vox" &&
        interaction.subcommand_name == "disconnect")
      return InteractionOperation::vox_test_disconnect;
    if (interaction.subcommand_group_name == "vox" &&
        interaction.subcommand_name == "speech-test")
      return InteractionOperation::vox_speech_test;
    if (interaction.subcommand_group_name == "vox" &&
        interaction.subcommand_name == "narration-preview")
      return InteractionOperation::vox_narration_preview;
    if (interaction.subcommand_group_name == "vox" &&
        interaction.subcommand_name == "narration-enqueue")
      return InteractionOperation::vox_narration_enqueue;
    if (interaction.subcommand_group_name == "vox" &&
        interaction.subcommand_name == "narration-recent")
      return InteractionOperation::vox_narration_recent;
    if (interaction.subcommand_group_name == "tarot") {
      if (interaction.subcommand_name == "adjust")
        return InteractionOperation::tarot_adjust;
      if (interaction.subcommand_name == "reverse")
        return InteractionOperation::tarot_reverse;
      if (interaction.subcommand_name == "economy")
        return InteractionOperation::tarot_economy;
      if (interaction.subcommand_name == "draw-test")
        return InteractionOperation::tarot_draw_test;
      if (interaction.subcommand_name == "draw-replay")
        return InteractionOperation::tarot_draw_replay;
      if (interaction.subcommand_name == "house-offer")
        return InteractionOperation::tarot_house_offer;
      if (interaction.subcommand_name == "house-resolve")
        return InteractionOperation::tarot_house_resolve;
      if (interaction.subcommand_name == "house-deadline")
        return InteractionOperation::tarot_house_deadline;
      if (interaction.subcommand_name == "house-cleanup")
        return InteractionOperation::tarot_house_cleanup;
      if (interaction.subcommand_name == "integration-preview")
        return InteractionOperation::tarot_integration_preview;
      if (interaction.subcommand_name == "integration-retry")
        return InteractionOperation::tarot_integration_retry;
      if (interaction.subcommand_name == "wager-role")
        return InteractionOperation::wager_test_role;
      if (interaction.subcommand_name == "wager-deadline")
        return InteractionOperation::wager_test_deadline;
      if (interaction.subcommand_name == "wager-cleanup")
        return InteractionOperation::wager_test_cleanup;
    }
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
    if (interaction.subcommand_group_name == "appearance") {
      if (interaction.subcommand_name == "simulate")
        return InteractionOperation::appearance_simulate;
      if (interaction.subcommand_name == "preview")
        return InteractionOperation::appearance_preview;
      if (interaction.subcommand_name == "recent")
        return InteractionOperation::appearance_recent;
      if (interaction.subcommand_name == "trigger")
        return InteractionOperation::appearance_trigger;
    }
  }
  if (interaction.command_name == "vox") {
    if (interaction.subcommand_name == "summon")
      return InteractionOperation::vox_summon;
    if (interaction.subcommand_name == "status")
      return InteractionOperation::vox_status;
    if (interaction.subcommand_name == "leave")
      return InteractionOperation::vox_leave;
    if (interaction.subcommand_name == "say")
      return InteractionOperation::vox_say;
    if (interaction.subcommand_name == "mute")
      return InteractionOperation::vox_mute;
    if (interaction.subcommand_name == "voice")
      return InteractionOperation::vox_voice;
    if (interaction.subcommand_name == "listen-start")
      return InteractionOperation::vox_listen_start;
    if (interaction.subcommand_name == "listen-stop")
      return InteractionOperation::vox_listen_stop;
  }
  if (interaction.command_name == "tarot") {
    if (interaction.subcommand_group_name == "house") {
      if (interaction.subcommand_name == "offers")
        return InteractionOperation::tarot_house_offers;
      if (interaction.subcommand_name == "play")
        return InteractionOperation::tarot_house_play;
      if (interaction.subcommand_name == "history")
        return InteractionOperation::tarot_house_history;
    }
    if (interaction.subcommand_name == "balance")
      return InteractionOperation::tarot_balance;
    if (interaction.subcommand_name == "history")
      return InteractionOperation::tarot_history;
    if (interaction.subcommand_name == "standings")
      return InteractionOperation::tarot_standings;
    if (interaction.subcommand_name == "standings-visibility")
      return InteractionOperation::tarot_standings_visibility;
    if (interaction.subcommand_name == "grace")
      return InteractionOperation::tarot_grace;
    if (interaction.subcommand_name == "trial")
      return InteractionOperation::tarot_trial;
    if (interaction.subcommand_name == "draw")
      return InteractionOperation::tarot_draw;
    if (interaction.subcommand_name == "record")
      return InteractionOperation::tarot_record;
    if (interaction.subcommand_name == "wager")
      return InteractionOperation::wager_create;
    if (interaction.subcommand_name == "wagers")
      return InteractionOperation::wager_history;
    if (interaction.subcommand_name == "wager-action")
      return InteractionOperation::wager_action;
    if (interaction.subcommand_name == "outcome")
      return InteractionOperation::wager_outcome;
    if (interaction.subcommand_name == "evidence")
      return InteractionOperation::wager_evidence;
    if (interaction.subcommand_name == "judgment")
      return InteractionOperation::wager_judgment;
    if (interaction.subcommand_name == "disputes")
      return InteractionOperation::wager_disputes;
  }
  if (interaction.command_name == "chronicle") {
    if (interaction.subcommand_group_name == "session") {
      if (interaction.subcommand_name == "start")
        return InteractionOperation::chronicle_session_start;
      if (interaction.subcommand_name == "status")
        return InteractionOperation::chronicle_session_status;
      if (interaction.subcommand_name == "close")
        return InteractionOperation::chronicle_session_close;
      if (interaction.subcommand_name == "edit")
        return InteractionOperation::chronicle_summary_edit;
      if (interaction.subcommand_name == "approve")
        return InteractionOperation::chronicle_summary_approve;
      if (interaction.subcommand_name == "reject")
        return InteractionOperation::chronicle_summary_reject;
    }
    if (interaction.subcommand_group_name == "title") {
      if (interaction.subcommand_name == "propose")
        return InteractionOperation::chronicle_title_propose;
      if (interaction.subcommand_name == "list")
        return InteractionOperation::chronicle_title_list;
      if (interaction.subcommand_name == "approve")
        return InteractionOperation::chronicle_title_approve;
      if (interaction.subcommand_name == "reject")
        return InteractionOperation::chronicle_title_reject;
      if (interaction.subcommand_name == "feature")
        return InteractionOperation::chronicle_title_feature;
      if (interaction.subcommand_name == "revoke")
        return InteractionOperation::chronicle_title_revoke;
    }
    if (interaction.subcommand_group_name == "anniversaries") {
      if (interaction.subcommand_name == "on")
        return InteractionOperation::chronicle_anniversaries_on;
      if (interaction.subcommand_name == "off")
        return InteractionOperation::chronicle_anniversaries_off;
    }
    if (interaction.subcommand_name == "recall")
      return InteractionOperation::chronicle_recall;
    if (interaction.subcommand_name == "timeline")
      return InteractionOperation::chronicle_timeline;
    if (interaction.subcommand_name == "forget")
      return InteractionOperation::chronicle_forget;
    if (interaction.subcommand_name == "profile")
      return InteractionOperation::chronicle_profile;
    if (interaction.subcommand_name == "callbacks")
      return InteractionOperation::chronicle_callbacks;
  }
  return std::nullopt;
}

[[nodiscard]] bool valid_slash_shape(const IncomingInteraction &interaction) {
  const auto catalog = command_catalog(true, true, true, true, true);
  const auto command = std::ranges::find(
      catalog.commands, interaction.command_name, &CommandDefinition::name);
  if (command == catalog.commands.end() ||
      command->kind != ApplicationCommandKind::chat_input)
    return false;
  const std::vector<CommandOptionDefinition> *expected_options = nullptr;
  if (interaction.subcommand_group_name.empty() &&
      interaction.subcommand_name.empty() && command->subcommands.empty() &&
      command->subcommand_groups.empty()) {
    expected_options = &command->options;
  } else if (interaction.subcommand_group_name.empty()) {
    const auto found =
        std::ranges::find(command->subcommands, interaction.subcommand_name,
                          &CommandSubcommandDefinition::name);
    if (found != command->subcommands.end())
      expected_options = &found->options;
  } else {
    const auto group = std::ranges::find(
        command->subcommand_groups, interaction.subcommand_group_name,
        &CommandSubcommandGroupDefinition::name);
    if (group == command->subcommand_groups.end())
      return false;
    const auto found =
        std::ranges::find(group->subcommands, interaction.subcommand_name,
                          &CommandSubcommandDefinition::name);
    if (found != group->subcommands.end())
      expected_options = &found->options;
  }
  if (expected_options == nullptr ||
      interaction.command_options.size() > expected_options->size())
    return false;
  for (const auto &expected : *expected_options) {
    const auto count = std::count_if(
        interaction.command_options.begin(), interaction.command_options.end(),
        [&expected](const InteractionOption &option) {
          return option.name == expected.name;
        });
    if ((expected.required && count != 1) || (!expected.required && count > 1))
      return false;
    if (count == 0)
      continue;
    const auto found = std::ranges::find(
        interaction.command_options, expected.name, &InteractionOption::name);
    if (expected.kind == CommandOptionKind::user) {
      const auto *value = std::get_if<DiscordId>(&found->value);
      if (value == nullptr || !value->is_set())
        return false;
    } else if (expected.kind == CommandOptionKind::integer) {
      const auto *value = std::get_if<std::int64_t>(&found->value);
      if (value == nullptr || !expected.minimum_integer ||
          !expected.maximum_integer || *value < *expected.minimum_integer ||
          *value > *expected.maximum_integer)
        return false;
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
                     [expected_options](const InteractionOption &option) {
                       return std::ranges::any_of(
                           *expected_options, [&option](const auto &expected) {
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
        features{features_value}, handler{handler_value},
        diagnostics{diagnostics_value} {}

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
    reply_ephemeral(interaction, presentation::ErrorKind::wrong_scope);
    return;
  }

  std::optional<InteractionOperation> operation;
  if (interaction.kind == InteractionKind::slash_command) {
    if (!interaction.custom_id.empty() ||
        !interaction.selected_values.empty() ||
        !interaction.modal_fields.empty() || interaction.context_message) {
      reply_ephemeral(interaction, presentation::ErrorKind::malformed);
      return;
    }
    if (!valid_slash_shape(interaction)) {
      reply_ephemeral(interaction, presentation::ErrorKind::malformed);
      return;
    }
    if (interaction.command_name == "chronicle" &&
        interaction.subcommand_name == "remember") {
      if (!state_->features.chronicle_enabled ||
          !interaction.command_options.empty()) {
        reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
        return;
      }
      interaction.responder->show_modal(ChronicleService::remember_modal());
      return;
    }
    operation = slash_operation(interaction);
    if (!operation.has_value()) {
      reply_ephemeral(interaction, presentation::ErrorKind::malformed);
      return;
    }
  } else if (interaction.kind == InteractionKind::message_context_command) {
    if (!state_->features.chronicle_enabled ||
        interaction.command_name != "Canonize in the Chronicle" ||
        !interaction.subcommand_name.empty() ||
        !interaction.command_options.empty() ||
        !interaction.custom_id.empty() ||
        !interaction.context_message.has_value()) {
      reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
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
      reply_ephemeral(interaction, presentation::ErrorKind::malformed);
      return;
    }
    if (interaction.kind == InteractionKind::modal_submit &&
        interaction.custom_id == "chronicle.remember:1") {
      if (!state_->features.chronicle_enabled) {
        reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
        return;
      }
      operation = InteractionOperation::chronicle_memory_preview;
    } else if (interaction.kind == InteractionKind::button) {
      if (const auto form_token = parse_wager_form(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        interaction.responder->show_modal(
            TarotWagerService::wager_form(*form_token));
        return;
      }
      if (const auto evidence_token =
              parse_wager_evidence_form(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        interaction.responder->show_modal(
            TarotWagerService::evidence_form(*evidence_token));
        return;
      }
      if (const auto outcome_token =
              parse_wager_outcome_form(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        interaction.responder->show_modal(
            TarotWagerService::outcome_form(*outcome_token));
        return;
      }
      if (const auto edit_token = parse_chronicle_component(
              interaction.custom_id, chronicle_session_edit_prefix)) {
        if (!state_->features.chronicle_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        interaction.responder->show_modal(
            ChronicleSessionService::summary_edit_modal(*edit_token));
        return;
      }
      if (const auto edit_token = parse_chronicle_component(
              interaction.custom_id, chronicle_modal_prefix)) {
        if (!state_->features.chronicle_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        interaction.responder->show_modal(
            ChronicleService::edit_entry_modal(*edit_token));
        return;
      }
      if (parse_voice_control(interaction.custom_id,
                              voice_transcript_component_prefix)) {
        if (!state_->features.chronicle_enabled ||
            !state_->handler.show_voice_transcript_modal(interaction)) {
          reply_ephemeral(interaction, presentation::ErrorKind::expired,
                          "/vox listen-start");
        }
        return;
      }
      if (parse_component_token(interaction.custom_id)) {
        operation = InteractionOperation::open_component;
      } else if (parse_wager_history(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        operation = InteractionOperation::wager_history;
      } else if (parse_wager_component(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        operation = InteractionOperation::wager_component;
      } else if (interaction.custom_id.starts_with(
                     tarot_house_history_page_prefix)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        operation = InteractionOperation::tarot_house_history;
      } else if (parse_tarot_house_component(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        operation = InteractionOperation::tarot_house_component;
      } else if (parse_tarot_component(interaction.custom_id)) {
        if (!state_->features.tarot_enabled) {
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        operation = InteractionOperation::tarot_component;
      } else if (interaction.custom_id.starts_with(
                     appearance_feedback_component_prefix) &&
                 valid_uuid_v4(interaction.custom_id.substr(
                     appearance_feedback_component_prefix.size()))) {
        operation = InteractionOperation::appearance_feedback_component;
      } else if (interaction.custom_id.starts_with(
                     chronicle_title_page_prefix)) {
        operation = InteractionOperation::chronicle_title_list;
      } else if (interaction.custom_id.starts_with(
                     chronicle_search_page_prefix) ||
                 parse_chronicle_component(interaction.custom_id,
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
          reply_ephemeral(interaction,
                          presentation::ErrorKind::feature_disabled);
          return;
        }
        operation = InteractionOperation::chronicle_component;
      } else if (parse_voice_control(interaction.custom_id,
                                     voice_listening_stop_prefix)) {
        operation = InteractionOperation::vox_listen_stop;
      }
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_wager_form(interaction.custom_id)) {
      operation = InteractionOperation::wager_preview;
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_wager_evidence_form(interaction.custom_id)) {
      operation = InteractionOperation::wager_evidence;
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_wager_outcome_form(interaction.custom_id)) {
      operation = InteractionOperation::wager_outcome;
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_chronicle_component(interaction.custom_id,
                                         chronicle_session_edit_prefix)) {
      operation = InteractionOperation::chronicle_summary_component;
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_chronicle_component(interaction.custom_id,
                                         chronicle_modal_prefix)) {
      operation = InteractionOperation::chronicle_edit;
    } else if (interaction.kind == InteractionKind::modal_submit &&
               parse_voice_control(interaction.custom_id,
                                   voice_transcript_modal_prefix)) {
      operation = InteractionOperation::vox_transcript_propose;
    }
    if (!operation.has_value()) {
      reply_ephemeral(interaction, presentation::ErrorKind::stale);
      return;
    }
  } else {
    reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
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
  if ((chronicle_operation || chronicle_session_operation ||
       anniversary_test) &&
      !state_->features.chronicle_enabled) {
    reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
    return;
  }

  const bool tarot_operation =
      *operation >= InteractionOperation::tarot_balance &&
      *operation <= InteractionOperation::wager_test_cleanup;
  if (tarot_operation && !state_->features.tarot_enabled) {
    reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
    return;
  }
  const bool vox_operation =
      *operation == InteractionOperation::vox_summon ||
      *operation == InteractionOperation::vox_status ||
      *operation == InteractionOperation::vox_leave ||
      *operation == InteractionOperation::vox_say ||
      *operation == InteractionOperation::vox_mute ||
      *operation == InteractionOperation::vox_voice ||
      *operation == InteractionOperation::vox_test_disconnect ||
      *operation == InteractionOperation::vox_speech_test ||
      *operation == InteractionOperation::vox_narration_preview ||
      *operation == InteractionOperation::vox_narration_enqueue ||
      *operation == InteractionOperation::vox_narration_recent ||
      *operation == InteractionOperation::vox_listen_start ||
      *operation == InteractionOperation::vox_listen_stop ||
      *operation == InteractionOperation::vox_transcript_propose;
  if (vox_operation && !state_->features.vox_enabled) {
    reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
    return;
  }

  const bool admin_operation =
      *operation == InteractionOperation::admin_health ||
      *operation == InteractionOperation::safety_status ||
      *operation == InteractionOperation::safety_set ||
      *operation == InteractionOperation::work_recent ||
      *operation == InteractionOperation::work_dead ||
      *operation == InteractionOperation::test_notice ||
      *operation == InteractionOperation::test_schedule_notice ||
      *operation == InteractionOperation::test_public_retry ||
      *operation == InteractionOperation::reliability_test ||
      *operation == InteractionOperation::appearance_simulate ||
      *operation == InteractionOperation::appearance_preview ||
      *operation == InteractionOperation::appearance_recent ||
      *operation == InteractionOperation::appearance_trigger ||
      *operation == InteractionOperation::tarot_adjust ||
      *operation == InteractionOperation::tarot_reverse ||
      *operation == InteractionOperation::tarot_economy ||
      *operation == InteractionOperation::tarot_draw_test ||
      *operation == InteractionOperation::tarot_draw_replay ||
      *operation == InteractionOperation::tarot_house_offer ||
      *operation == InteractionOperation::tarot_house_resolve ||
      *operation == InteractionOperation::tarot_house_deadline ||
      *operation == InteractionOperation::tarot_house_cleanup ||
      *operation == InteractionOperation::tarot_integration_preview ||
      *operation == InteractionOperation::tarot_integration_retry ||
      *operation == InteractionOperation::wager_test_role ||
      *operation == InteractionOperation::wager_test_deadline ||
      *operation == InteractionOperation::wager_test_cleanup ||
      *operation == InteractionOperation::vox_narration_preview ||
      *operation == InteractionOperation::vox_narration_enqueue ||
      *operation == InteractionOperation::vox_narration_recent ||
      *operation == InteractionOperation::vox_listening_disable ||
      *operation == InteractionOperation::vox_listening_enable;
  const bool vox_test_operation =
      *operation == InteractionOperation::vox_test_disconnect ||
      *operation == InteractionOperation::vox_speech_test ||
      *operation == InteractionOperation::vox_narration_enqueue;
  const bool appearance_safety_operation =
      *operation == InteractionOperation::appearance_disable ||
      *operation == InteractionOperation::appearance_enable;
  const bool unified_safety_operation =
      *operation == InteractionOperation::safety_status ||
      *operation == InteractionOperation::safety_set;
  const bool voice_safety_operation =
      *operation == InteractionOperation::vox_listening_disable ||
      *operation == InteractionOperation::vox_listening_enable;
  const bool test_operation =
      *operation == InteractionOperation::test_notice ||
      *operation == InteractionOperation::test_schedule_notice ||
      *operation == InteractionOperation::test_public_retry ||
      *operation == InteractionOperation::reliability_test ||
      *operation == InteractionOperation::appearance_simulate ||
      *operation == InteractionOperation::appearance_trigger ||
      *operation == InteractionOperation::tarot_adjust ||
      *operation == InteractionOperation::tarot_reverse ||
      *operation == InteractionOperation::tarot_draw_test ||
      *operation == InteractionOperation::tarot_house_offer ||
      *operation == InteractionOperation::tarot_house_deadline ||
      *operation == InteractionOperation::tarot_house_cleanup ||
      *operation == InteractionOperation::tarot_integration_retry ||
      *operation == InteractionOperation::wager_test_role ||
      *operation == InteractionOperation::wager_test_deadline ||
      *operation == InteractionOperation::wager_test_cleanup;
  if (admin_operation || anniversary_test || appearance_safety_operation ||
      unified_safety_operation || voice_safety_operation ||
      vox_test_operation) {
    if (!appearance_safety_operation && !unified_safety_operation &&
        !voice_safety_operation && !state_->controls.admin_commands_enabled) {
      state_->diagnostics.emit(
          {DiagnosticSeverity::warning, "interaction.admin_rejected",
           "Owner interaction was rejected because administration is disabled.",
           interaction.correlation_id});
      reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
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
      reply_ephemeral(interaction, presentation::ErrorKind::forbidden);
      return;
    }
    if ((test_operation || anniversary_test || vox_test_operation) &&
        !state_->controls.test_mode) {
      state_->diagnostics.emit(
          {DiagnosticSeverity::warning, "interaction.admin_rejected",
           "Owner test interaction was rejected because test mode is disabled.",
           interaction.correlation_id});
      reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
      return;
    }
    if (*operation == InteractionOperation::appearance_simulate &&
        state_->features.appearances_mode != AppearanceMode::dry_run) {
      reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
      return;
    }
    if (*operation == InteractionOperation::appearance_trigger &&
        state_->features.appearances_mode != AppearanceMode::live) {
      reply_ephemeral(interaction, presentation::ErrorKind::feature_disabled);
      return;
    }
  }

  auto shared_state = state_;
  bool public_profile = false;
  if (*operation == InteractionOperation::chronicle_profile &&
      !interaction.command_options.empty()) {
    if (const auto *target = std::get_if<DiscordId>(
            &interaction.command_options.front().value)) {
      public_profile = *target != interaction.user_id;
    }
  }
  const bool privacy_control =
      *operation == InteractionOperation::vox_listen_stop ||
      *operation == InteractionOperation::vox_listening_disable ||
      *operation == InteractionOperation::vox_listening_enable;
  const bool emergency_privacy_control =
      *operation == InteractionOperation::vox_listen_stop ||
      *operation == InteractionOperation::vox_listening_disable;
  auto queued = RoutedInteraction{std::move(interaction), *operation};
  if (privacy_control) {
    const std::scoped_lock lock{state_->mutex};
    if (!state_->accepting)
      return;
    state_->handler.preempt_voice_privacy_control(queued);
  }
  queued.interaction.responder->defer(
      (*operation == InteractionOperation::chronicle_timeline ||
       public_profile || *operation == InteractionOperation::tarot_standings ||
       *operation == InteractionOperation::vox_status)
          ? ResponseVisibility::public_message
          : ResponseVisibility::ephemeral,
      [shared_state, privacy_control, emergency_privacy_control,
       queued = std::move(queued)](const DeliveryResult result) mutable {
        if (result != DeliveryResult::success) {
          shared_state->diagnostics.emit(
              {DiagnosticSeverity::warning, "interaction.defer",
               "Discord did not confirm the interaction acknowledgement.",
               queued.interaction.correlation_id});
          if (!emergency_privacy_control)
            return;
        }
        const std::scoped_lock lock{shared_state->mutex};
        if (shared_state->accepting) {
          if (privacy_control) {
            static_cast<void>(shared_state->handler.enqueue_privacy_control(
                std::move(queued)));
          } else {
            static_cast<void>(shared_state->handler.enqueue(std::move(queued)));
          }
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
