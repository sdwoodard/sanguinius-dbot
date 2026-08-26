#include "sanguinius/dpp_discord_adapter.hpp"

#include "sanguinius/ai_client.hpp"
#include "sanguinius/callback_fence.hpp"
#include "sanguinius/chronicle.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/dpp_command_registry.hpp"
#include "sanguinius/dpp_cluster_host.hpp"
#include "sanguinius/interaction_response_state.hpp"

#include <dpp/dpp.h>
#include <dpp/restrequest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sanguinius {
namespace {

constexpr auto cancellation_poll_interval = std::chrono::milliseconds{25};

[[nodiscard]] dpp::cluster &
require_cluster(const std::shared_ptr<DppClusterHost> &cluster_host) {
  if (!cluster_host)
    throw std::invalid_argument{"Discord adapter requires a cluster host."};
  return cluster_host->native();
}

[[nodiscard]] DiscordId id(const dpp::snowflake value) {
  return DiscordId{static_cast<std::uint64_t>(value)};
}

[[nodiscard]] ConversationEntry
conversation_entry(const dpp::message &message) {
  return ConversationEntry{
      .message_id = id(message.id),
      .author_username = message.author.username,
      .author_display_name = message.author.global_name,
      .content = message.content,
  };
}

[[nodiscard]] IncomingMessage incoming_message(const dpp::message &message,
                                               const DiscordId bot_user_id) {
  IncomingMessage incoming{
      .correlation_id = {},
      .bot_user_id = bot_user_id,
      .message_id = id(message.id),
      .guild_id = id(message.guild_id),
      .channel_id = id(message.channel_id),
      .author_user_id = id(message.author.id),
      .author_username = message.author.username,
      .author_display_name = message.author.global_name,
      .content = message.content,
      .author_is_bot = message.author.is_bot(),
      .replied_to = std::nullopt,
  };

  if (message.message_reference.message_id != 0) {
    incoming.replied_to = MessageReference{
        .message_id = id(message.message_reference.message_id),
        .guild_id = id(message.message_reference.guild_id),
        .channel_id = id(message.message_reference.channel_id),
    };
  }
  return incoming;
}

[[nodiscard]] DiagnosticSeverity severity(const dpp::loglevel level) {
  switch (level) {
  case dpp::ll_trace:
  case dpp::ll_debug:
    return DiagnosticSeverity::debug;
  case dpp::ll_info:
    return DiagnosticSeverity::info;
  case dpp::ll_warning:
    return DiagnosticSeverity::warning;
  case dpp::ll_error:
  case dpp::ll_critical:
    return DiagnosticSeverity::error;
  }
  return DiagnosticSeverity::info;
}

[[nodiscard]] DeliveryResult
delivery_result(const dpp::confirmation_callback_t &confirmation) noexcept {
  return dpp_adapter_detail::classify_http_delivery(
      !confirmation.is_error(), confirmation.http_info.status,
      confirmation.http_info.error != dpp::h_success);
}

template <typename Callback>
void invoke_callback(const std::shared_ptr<CallbackFence> &callbacks,
                     Callback &&callback) noexcept {
  try {
    static_cast<void>(callbacks->invoke(
        std::function<void()>{std::forward<Callback>(callback)}));
  } catch (...) {
  }
}

[[nodiscard]] dpp::message discord_message(const InteractionMessage &source) {
  if (source.content.size() > 2'000 || source.buttons.size() > 5 ||
      source.allowed_user_mentions.size() > 1 ||
      (source.embed.has_value() &&
       (source.embed->title.size() > 256 ||
        source.embed->description.size() > 4'096))) {
    throw std::invalid_argument{
        "Discord interaction message exceeds safe bounds."};
  }
  dpp::message message;
  message.set_content(source.content);
  if (source.embed.has_value()) {
    dpp::embed embed;
    embed.set_color(source.embed->color)
        .set_title(source.embed->title)
        .set_description(source.embed->description);
    if (!source.embed->url.empty()) {
      embed.set_url(source.embed->url);
    }
    message.add_embed(embed);
  }
  if (!source.buttons.empty()) {
    dpp::component row;
    row.set_type(dpp::cot_action_row);
    for (const auto &source_button : source.buttons) {
      if (source_button.custom_id.empty() ||
          source_button.custom_id.size() > maximum_interaction_name_size ||
          source_button.label.empty() || source_button.label.size() > 80) {
        throw std::invalid_argument{
            "Discord interaction button exceeds safe bounds."};
      }
      dpp::component button;
      button.set_type(dpp::cot_button)
          .set_style(source_button.style == ButtonStyle::secondary
                         ? dpp::cos_secondary
                         : dpp::cos_primary)
          .set_label(source_button.label)
          .set_id(source_button.custom_id)
          .set_disabled(source_button.disabled);
      row.add_component(button);
    }
    message.add_component(row);
  }
  std::vector<dpp::snowflake> mentioned_users;
  mentioned_users.reserve(source.allowed_user_mentions.size());
  for (const auto user_id : source.allowed_user_mentions) {
    mentioned_users.emplace_back(user_id.value());
  }
  message.set_allowed_mentions(false, false, false, false, mentioned_users, {});
  return message;
}

[[nodiscard]] dpp::interaction_modal_response
discord_modal(const ModalPayload &source) {
  if (source.custom_id.empty() ||
      source.custom_id.size() > maximum_interaction_name_size ||
      source.title.empty() || source.title.size() > 45 ||
      source.fields.empty() || source.fields.size() > maximum_modal_fields) {
    throw std::invalid_argument{"Discord modal exceeds safe bounds."};
  }
  dpp::interaction_modal_response modal{source.custom_id, source.title};
  for (const auto &source_field : source.fields) {
    if (source_field.custom_id.empty() ||
        source_field.custom_id.size() > maximum_interaction_name_size ||
        source_field.label.empty() || source_field.label.size() > 45 ||
        source_field.minimum_length > source_field.maximum_length ||
        source_field.maximum_length > maximum_modal_value_size ||
        source_field.value.size() > source_field.maximum_length) {
      throw std::invalid_argument{"Discord modal field exceeds safe bounds."};
    }
    dpp::component field;
    field.set_type(dpp::cot_text)
        .set_text_style(source_field.style ==
                                ModalFieldPayload::Style::paragraph
                            ? dpp::text_paragraph
                            : dpp::text_short)
        .set_id(source_field.custom_id)
        .set_label(source_field.label)
        .set_default_value(source_field.value)
        .set_min_length(static_cast<std::uint32_t>(source_field.minimum_length))
        .set_max_length(static_cast<std::uint32_t>(source_field.maximum_length))
        .set_required(source_field.required);
    dpp::component row;
    row.set_type(dpp::cot_action_row).add_component(field);
    modal.add_component(row);
  }
  return modal;
}

[[nodiscard]] dpp::command_completion_event_t
completion(const std::shared_ptr<CallbackFence> &callbacks,
           DeliveryCallback callback) {
  return [callbacks, callback = std::move(callback)](
             const dpp::confirmation_callback_t &confirmation) {
    invoke_callback(callbacks, [&callback, &confirmation] {
      if (callback) {
        callback(delivery_result(confirmation));
      }
    });
  };
}

class DppInteractionResponder final : public DiscordInteractionResponder {
public:
  DppInteractionResponder(const dpp::interaction_create_t &event,
                          std::shared_ptr<CallbackFence> callbacks)
      : event_{event}, callbacks_{std::move(callbacks)} {}

  void reply(const InteractionMessage &source,
             const ResponseVisibility visibility,
             DeliveryCallback callback) override {
    dpp::message message;
    try {
      message = discord_message(source);
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    bool accepted;
    {
      const std::scoped_lock lock{mutex_};
      accepted = state_.acknowledge_reply(visibility);
    }
    if (!accepted) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    if (visibility == ResponseVisibility::ephemeral) {
      message.set_flags(dpp::m_ephemeral);
    }
    auto asynchronous_callback = callback;
    try {
      event_.reply(message,
                   completion(callbacks_, std::move(asynchronous_callback)));
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::unknown_outcome);
      }
    }
  }

  void defer(const ResponseVisibility visibility,
             DeliveryCallback callback) override {
    bool accepted;
    {
      const std::scoped_lock lock{mutex_};
      accepted = state_.acknowledge_defer(visibility);
    }
    if (!accepted) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    auto asynchronous_callback = callback;
    try {
      event_.thinking(visibility == ResponseVisibility::ephemeral,
                      completion(callbacks_, std::move(asynchronous_callback)));
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::unknown_outcome);
      }
    }
  }

  void edit_original(const InteractionMessage &source,
                     DeliveryCallback callback) override {
    dpp::message message;
    try {
      message = discord_message(source);
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    bool accepted;
    {
      const std::scoped_lock lock{mutex_};
      accepted = state_.may_edit();
    }
    if (!accepted) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    auto asynchronous_callback = callback;
    try {
      event_.edit_response(
          message, completion(callbacks_, std::move(asynchronous_callback)));
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::unknown_outcome);
      }
    }
  }

  void follow_up(const InteractionMessage &source,
                 const ResponseVisibility visibility,
                 DeliveryCallback callback) override {
    dpp::message message;
    try {
      message = discord_message(source);
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    bool accepted;
    {
      const std::scoped_lock lock{mutex_};
      accepted = state_.may_follow_up(visibility);
    }
    if (!accepted) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    if (visibility == ResponseVisibility::ephemeral) {
      message.set_flags(dpp::m_ephemeral);
    }
    auto asynchronous_callback = callback;
    try {
      event_.follow_up(
          message, completion(callbacks_, std::move(asynchronous_callback)));
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::unknown_outcome);
      }
    }
  }

  void show_modal(const ModalPayload &source,
                  DeliveryCallback callback) override {
    std::optional<dpp::interaction_modal_response> modal;
    try {
      modal = discord_modal(source);
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    bool accepted;
    {
      const std::scoped_lock lock{mutex_};
      accepted = state_.acknowledge_modal();
    }
    if (!accepted) {
      if (callback) {
        callback(DeliveryResult::permanent_failure);
      }
      return;
    }
    auto asynchronous_callback = callback;
    try {
      event_.dialog(*modal,
                    completion(callbacks_, std::move(asynchronous_callback)));
    } catch (...) {
      if (callback) {
        callback(DeliveryResult::unknown_outcome);
      }
    }
  }

private:
  dpp::interaction_create_t event_;
  std::shared_ptr<CallbackFence> callbacks_;
  std::mutex mutex_;
  InteractionResponseState state_;
};

[[nodiscard]] IncomingInteraction
incoming_interaction(const dpp::interaction_create_t &event,
                     const InteractionKind kind,
                     std::shared_ptr<CallbackFence> callbacks) {
  const auto &user = event.command.get_issuing_user();
  auto display_name = event.command.member.get_nickname();
  if (display_name.empty()) {
    display_name = user.global_name.empty() ? user.username : user.global_name;
  }
  IncomingInteraction result{
      .correlation_id = {},
      .interaction_id = id(event.command.id),
      .guild_id = id(event.command.guild_id),
      .channel_id = id(event.command.channel_id),
      .user_id = id(user.id),
      .username = user.username,
      .display_name = std::move(display_name),
      .kind = kind,
      .command_name = {},
      .subcommand_group_name = {},
      .subcommand_name = {},
      .command_options = {},
      .resolved_users = {},
      .custom_id = {},
      .selected_values = {},
      .modal_fields = {},
      .context_message = std::nullopt,
      .responder = std::make_shared<DppInteractionResponder>(
          event, std::move(callbacks)),
  };
  if (event.command.resolved.users.size() > maximum_interaction_options)
    throw std::invalid_argument{"Discord resolved users exceed safe bounds."};
  for (const auto &[user_id, resolved_user] : event.command.resolved.users) {
    const auto member = event.command.resolved.members.find(user_id);
    auto resolved_display_name =
        member == event.command.resolved.members.end()
            ? std::string{}
            : member->second.get_nickname();
    if (resolved_display_name.empty())
      resolved_display_name = resolved_user.global_name.empty()
                                  ? resolved_user.username
                                  : resolved_user.global_name;
    if (resolved_user.username.empty() || resolved_user.username.size() > 128 ||
        resolved_display_name.empty() || resolved_display_name.size() > 128)
      throw std::invalid_argument{"Discord resolved user exceeds safe bounds."};
    result.resolved_users.push_back(ResolvedUserSnapshot{
        .user_id = id(user_id),
        .username = resolved_user.username,
        .display_name = std::move(resolved_display_name),
        .is_bot = resolved_user.is_bot(),
    });
  }
  return result;
}

void append_modal_fields(
    const dpp::component &component,
    std::vector<std::pair<std::string, std::string>> &fields,
    const std::size_t depth = 0) {
  if (depth > 2 || fields.size() > maximum_modal_fields) {
    throw std::invalid_argument{"Discord modal fields exceed safe bounds."};
  }
  if (component.type == dpp::cot_text && !component.custom_id.empty()) {
    if (const auto *value = std::get_if<std::string>(&component.value)) {
      if (fields.size() == maximum_modal_fields ||
          component.custom_id.size() > maximum_interaction_name_size ||
          value->size() > maximum_modal_value_size) {
        throw std::invalid_argument{"Discord modal fields exceed safe bounds."};
      }
      fields.emplace_back(component.custom_id, *value);
    }
  }
  for (const auto &child : component.components) {
    append_modal_fields(child, fields, depth + 1);
  }
}

[[nodiscard]] InteractionOptionValue
interaction_option_value(const dpp::command_value &value) {
  return std::visit(
      [](const auto &actual) -> InteractionOptionValue {
        using Value = std::decay_t<decltype(actual)>;
        if constexpr (std::is_same_v<Value, std::string>) {
          if (actual.size() > maximum_modal_value_size) {
            throw std::invalid_argument{
                "Discord command option exceeds safe bounds."};
          }
          return actual;
        } else if constexpr (std::is_same_v<Value, std::int64_t> ||
                             std::is_same_v<Value, bool> ||
                             std::is_same_v<Value, double>) {
          return actual;
        } else if constexpr (std::is_same_v<Value, dpp::snowflake>) {
          return DiscordId{static_cast<std::uint64_t>(actual)};
        } else {
          throw std::invalid_argument{"Discord command option has no value."};
        }
      },
      value);
}

void append_command_options(const std::vector<dpp::command_data_option> &source,
                            std::vector<InteractionOption> &destination) {
  if (source.size() > maximum_interaction_options) {
    throw std::invalid_argument{"Discord command options exceed safe bounds."};
  }
  for (const auto &option : source) {
    if (destination.size() == maximum_interaction_options ||
        option.name.empty() ||
        option.name.size() > maximum_interaction_name_size ||
        !option.options.empty()) {
      throw std::invalid_argument{
          "Discord command options exceed safe bounds."};
    }
    destination.push_back(
        InteractionOption{option.name, interaction_option_value(option.value)});
  }
}

[[nodiscard]] std::vector<dpp::slashcommand>
make_discord_commands(const CommandCatalog &catalog,
                      const dpp::snowflake application_id) {
  std::vector<dpp::slashcommand> result;
  result.reserve(catalog.commands.size());
  for (const auto &definition : catalog.commands) {
    dpp::slashcommand command;
    if (definition.kind == ApplicationCommandKind::message_context) {
      // D++ 10.1.7's context-command constructor applies chat-input
      // lowercasing before it assigns the context type. Set the type first so
      // Discord receives the intended human-facing context-menu name.
      command.set_type(dpp::ctxm_message)
          .set_name(definition.name)
          .set_application_id(application_id);
    } else {
      command = dpp::slashcommand{definition.name, definition.description,
                                  application_id};
    }
    command.set_dm_permission(false);
    for (const auto &subcommand : definition.subcommands) {
      dpp::command_option translated{dpp::co_sub_command, subcommand.name,
                                     subcommand.description};
      for (const auto &option : subcommand.options) {
        const auto option_type = option.kind == CommandOptionKind::user
                                     ? dpp::co_user
                                 : option.kind == CommandOptionKind::integer
                                     ? dpp::co_integer
                                     : dpp::co_string;
        dpp::command_option translated_option{
            option_type,
            option.name, option.description, option.required};
        if (option.kind == CommandOptionKind::string) {
          translated_option.set_min_length(
              static_cast<std::int64_t>(option.minimum_length));
          translated_option.set_max_length(
              static_cast<std::int64_t>(option.maximum_length));
        } else if (option.kind == CommandOptionKind::integer) {
          if (!option.minimum_integer || !option.maximum_integer)
            throw std::invalid_argument{"Integer command bounds are required."};
          translated_option.set_min_value(*option.minimum_integer);
          translated_option.set_max_value(*option.maximum_integer);
        }
        for (const auto &choice : option.choices) {
          translated_option.add_choice(
              dpp::command_option_choice{choice.name, choice.value});
        }
        translated.add_option(translated_option);
      }
      command.add_option(translated);
    }
    for (const auto &group : definition.subcommand_groups) {
      dpp::command_option translated_group{
          dpp::co_sub_command_group, group.name, group.description};
      for (const auto &subcommand : group.subcommands) {
        dpp::command_option translated{dpp::co_sub_command, subcommand.name,
                                       subcommand.description};
        for (const auto &option : subcommand.options) {
          const auto option_type = option.kind == CommandOptionKind::user
                                       ? dpp::co_user
                                   : option.kind == CommandOptionKind::integer
                                       ? dpp::co_integer
                                       : dpp::co_string;
          dpp::command_option translated_option{
              option_type,
              option.name, option.description, option.required};
          if (option.kind == CommandOptionKind::string) {
            translated_option.set_min_length(
                static_cast<std::int64_t>(option.minimum_length));
            translated_option.set_max_length(
                static_cast<std::int64_t>(option.maximum_length));
          } else if (option.kind == CommandOptionKind::integer) {
            if (!option.minimum_integer || !option.maximum_integer)
              throw std::invalid_argument{
                  "Integer command bounds are required."};
            translated_option.set_min_value(*option.minimum_integer);
            translated_option.set_max_value(*option.maximum_integer);
          }
          for (const auto &choice : option.choices)
            translated_option.add_choice(
                dpp::command_option_choice{choice.name, choice.value});
          translated.add_option(translated_option);
        }
        translated_group.add_option(translated);
      }
      command.add_option(translated_group);
    }
    result.push_back(std::move(command));
  }
  return result;
}

template <typename Value>
Value wait_for_discord(std::future<Value> &future,
                       const std::stop_token stop_token,
                       const std::chrono::seconds request_timeout,
                       const std::string_view timeout_message) {
  const auto deadline = std::chrono::steady_clock::now() + request_timeout;
  while (future.wait_for(cancellation_poll_interval) !=
         std::future_status::ready) {
    if (stop_token.stop_requested()) {
      throw OperationCancelled{};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error{std::string{timeout_message}};
    }
  }
  if (stop_token.stop_requested()) {
    throw OperationCancelled{};
  }
  return future.get();
}

} // namespace

DeliveryResult dpp_adapter_detail::classify_http_delivery(
    const bool succeeded, const int http_status,
    const bool transport_failed) noexcept {
  if (succeeded) {
    return DeliveryResult::success;
  }
  if (http_status == 0 || transport_failed) {
    return DeliveryResult::unknown_outcome;
  }
  if (http_status == 429 || http_status >= 500) {
    return DeliveryResult::transient_failure;
  }
  return DeliveryResult::permanent_failure;
}

DiscordId dpp_adapter_detail::provider_message_id(const dpp::message &message) {
  const auto result = id(message.id);
  if (!result.is_set()) {
    throw std::invalid_argument{"Discord delivery receipt has no message ID."};
  }
  return result;
}

ContextMessageSnapshot
dpp_adapter_detail::context_message_snapshot(const dpp::message &message) {
  if (message.mentions.size() > maximum_chronicle_mentions) {
    throw std::invalid_argument{
        "Discord message has too many Chronicle participants."};
  }
  const auto bounded = [](const std::string &value, const std::size_t maximum) {
    if (value.size() <= maximum)
      return value;
    auto end = maximum;
    while (end > 0 &&
           (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return value.substr(0, end);
  };
  ContextMessageSnapshot snapshot{
      .reference = MessageReference{.message_id = id(message.id),
                                    .guild_id = id(message.guild_id),
                                    .channel_id = id(message.channel_id)},
      .author =
          ContextUserSnapshot{
              .user_id = id(message.author.id),
              .username = bounded(message.author.username, 128),
              .display_name = bounded(!message.member.get_nickname().empty()
                                          ? message.member.get_nickname()
                                          : (!message.author.global_name.empty()
                                                 ? message.author.global_name
                                                 : message.author.username),
                                      128),
              .is_bot = message.author.is_bot()},
      .content = bounded(message.content, maximum_chronicle_source_size),
      .content_truncated =
          message.content.size() > maximum_chronicle_source_size,
      .occurred_at_ms = static_cast<std::int64_t>(message.sent) * 1'000,
      .mentioned_users = {},
      .attachments = {},
  };
  for (std::size_t index = 0; index < message.mentions.size(); ++index) {
    const auto &[user, member] = message.mentions[index];
    snapshot.mentioned_users.push_back(ContextUserSnapshot{
        .user_id = id(user.id),
        .username = bounded(user.username, 128),
        .display_name =
            bounded(!member.get_nickname().empty()
                        ? member.get_nickname()
                        : (!user.global_name.empty() ? user.global_name
                                                     : user.username),
                    128),
        .is_bot = user.is_bot()});
  }
  const auto attachment_count = std::min<std::size_t>(
      message.attachments.size(), maximum_chronicle_attachments);
  for (std::size_t index = 0; index < attachment_count; ++index) {
    const auto &attachment = message.attachments[index];
    snapshot.attachments.push_back(ContextAttachmentSnapshot{
        .attachment_id = id(attachment.id),
        .filename = bounded(attachment.filename, 255),
        .content_type =
            attachment.content_type.empty()
                ? std::nullopt
                : std::optional{bounded(attachment.content_type, 127)},
        .byte_size = attachment.size,
        .width = attachment.width == 0 ? std::nullopt
                                       : std::optional{attachment.width},
        .height = attachment.height == 0 ? std::nullopt
                                         : std::optional{attachment.height},
        .ephemeral = attachment.ephemeral,
        .spoiler = attachment.filename.starts_with("SPOILER_")});
  }
  return snapshot;
}

std::vector<dpp::slashcommand> dpp_adapter_detail::translate_command_catalog(
    const CommandCatalog &catalog, const dpp::snowflake application_id) {
  return make_discord_commands(catalog, application_id);
}

void dpp_adapter_detail::translate_slash_command(
    const dpp::command_interaction &command, IncomingInteraction &interaction) {
  const auto valid_name = [](const std::string_view name) {
    return !name.empty() && name.size() <= maximum_interaction_name_size;
  };
  if (!valid_name(command.name))
    throw std::invalid_argument{"Discord command name exceeds safe bounds."};
  interaction.command_name = command.name;
  if (command.options.empty())
    return;

  const auto &outer = command.options.front();
  if (command.options.size() == 1 && outer.type == dpp::co_sub_command) {
    if (!valid_name(outer.name))
      throw std::invalid_argument{"Discord subcommand name is invalid."};
    interaction.subcommand_name = outer.name;
    append_command_options(outer.options, interaction.command_options);
    return;
  }
  if (command.options.size() == 1 &&
      outer.type == dpp::co_sub_command_group) {
    if (!valid_name(outer.name) || outer.options.size() != 1 ||
        outer.options.front().type != dpp::co_sub_command ||
        !valid_name(outer.options.front().name)) {
      throw std::invalid_argument{"Discord subcommand group is malformed."};
    }
    interaction.subcommand_group_name = outer.name;
    interaction.subcommand_name = outer.options.front().name;
    append_command_options(outer.options.front().options,
                           interaction.command_options);
    return;
  }
  if (std::ranges::any_of(command.options, [](const auto &option) {
        return option.type == dpp::co_sub_command ||
               option.type == dpp::co_sub_command_group;
      })) {
    throw std::invalid_argument{"Discord command nesting is malformed."};
  }
  append_command_options(command.options, interaction.command_options);
}

std::string dpp_adapter_detail::durable_public_message_json(
    const PublicMessageRequest &request,
    const std::string_view provider_nonce) {
  if (!request.guild_id.is_set() || !request.channel_id.is_set()) {
    throw std::invalid_argument{"Discord public-message target is incomplete."};
  }
  if (provider_nonce.size() != 25 ||
      std::any_of(provider_nonce.begin(), provider_nonce.end(),
                  [](const char character) {
                    return !((character >= '0' && character <= '9') ||
                             (character >= 'a' && character <= 'f'));
                  })) {
    throw std::invalid_argument{"Discord provider nonce is invalid."};
  }
  auto message = discord_message(request.message);
  const auto serialized = message.to_json(false);
  nlohmann::json payload = nlohmann::json::object();
  if (const auto content = serialized.find("content");
      content != serialized.end() && content->is_string() &&
      !content->get_ref<const std::string &>().empty()) {
    payload["content"] = *content;
  }
  for (const auto *field : {"embeds", "components"}) {
    if (const auto value = serialized.find(field);
        value != serialized.end() && value->is_array() && !value->empty()) {
      payload[field] = *value;
    }
  }
  if (const auto allowed_mentions = serialized.find("allowed_mentions");
      allowed_mentions != serialized.end()) {
    payload["allowed_mentions"] = *allowed_mentions;
  }
  if (!payload.contains("content") && !payload.contains("embeds") &&
      !payload.contains("components")) {
    throw std::invalid_argument{"Discord public message is empty."};
  }
  payload["nonce"] = provider_nonce;
  payload["enforce_nonce"] = true;
  return payload.dump();
}

std::string_view
dpp_adapter_detail::durable_public_message_base_path() noexcept {
  return API_PATH "/channels";
}

int run_discord_command_operator(const DiscordCommandOperation operation,
                                 std::string token,
                                 const std::chrono::seconds request_timeout,
                                 const DiscordId guild_id,
                                 const CommandCatalog &catalog,
                                 std::ostream &output, std::ostream &errors) {
  if (!guild_id.is_set()) {
    errors << "Discord command operation requires a configured guild.\n";
    return 2;
  }

  struct OperatorResult {
    bool success{};
    bool changed{};
  };

  dpp::cluster bot{std::move(token), dpp::i_guilds};
  auto result = std::make_shared<std::promise<OperatorResult>>();
  auto future = result->get_future();
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto ready_started = std::make_shared<std::atomic<bool>>(false);
  auto callbacks = std::make_shared<CallbackFence>();
  const auto finish = [result, completed](const OperatorResult value) {
    if (!completed->exchange(true)) {
      try {
        result->set_value(value);
      } catch (...) {
      }
    }
  };

  bot.on_ready([&bot, operation, guild_id, catalog, finish, ready_started,
                callbacks](const dpp::ready_t &) {
    invoke_callback(callbacks, [&bot, operation, guild_id, catalog, finish,
                                ready_started, callbacks] {
      if (ready_started->exchange(true)) {
        return;
      }
      if (operation == DiscordCommandOperation::clear) {
        bot.guild_bulk_command_delete(
            dpp::snowflake{guild_id.value()},
            [finish,
             callbacks](const dpp::confirmation_callback_t &confirmation) {
              invoke_callback(callbacks, [&confirmation, finish] {
                finish({.success = !confirmation.is_error(), .changed = true});
              });
            });
        return;
      }

      const auto desired =
          dpp_adapter_detail::translate_command_catalog(catalog, bot.me.id);
      bot.guild_commands_get(
          dpp::snowflake{guild_id.value()},
          [&bot, guild_id, desired, finish,
           callbacks](const dpp::confirmation_callback_t &confirmation) {
            invoke_callback(callbacks, [&bot, guild_id, desired, finish,
                                        callbacks, &confirmation] {
              if (confirmation.is_error()) {
                finish({});
                return;
              }
              try {
                const auto existing = confirmation.get<dpp::slashcommand_map>();
                if (dpp_adapter_detail::commands_match(existing, desired)) {
                  finish({.success = true, .changed = false});
                  return;
                }
                bot.guild_bulk_command_create(
                    desired, dpp::snowflake{guild_id.value()},
                    [finish,
                     callbacks](const dpp::confirmation_callback_t &updated) {
                      invoke_callback(callbacks, [&updated, finish] {
                        finish(
                            {.success = !updated.is_error(), .changed = true});
                      });
                    });
              } catch (...) {
                finish({});
              }
            });
          });
    });
  });

  try {
    bot.start(dpp::st_return);
    if (future.wait_for(request_timeout) != std::future_status::ready) {
      errors << "Discord command operation timed out.\n";
      callbacks->close_and_wait();
      bot.shutdown();
      return 1;
    }
    const auto operation_result = future.get();
    callbacks->close_and_wait();
    bot.shutdown();
    if (!operation_result.success) {
      errors << "Discord command operation failed.\n";
      return 1;
    }
    if (operation == DiscordCommandOperation::clear) {
      output << "Guild command catalog cleared.\n";
    } else if (operation_result.changed) {
      output << "Guild command catalog synchronized.\n";
    } else {
      output << "Guild command catalog already synchronized.\n";
    }
    return 0;
  } catch (...) {
    try {
      callbacks->close_and_wait();
      bot.shutdown();
    } catch (...) {
    }
    errors << "Discord command operation failed.\n";
    return 1;
  }
}

class DppDiscordAdapter::Impl {
public:
  Impl(std::shared_ptr<DppClusterHost> cluster_host,
       const std::chrono::seconds request_timeout, const DiscordId guild_id,
      Diagnostics &diagnostics)
      : cluster_host_{std::move(cluster_host)},
        bot_{require_cluster(cluster_host_)}, diagnostics_{diagnostics},
        request_timeout_{request_timeout}, guild_id_{guild_id},
        callbacks_{std::make_shared<CallbackFence>()} {
    if (!guild_id_.is_set()) {
      throw std::invalid_argument{"Discord guild ID must be configured."};
    }
  }

  void start(MessageCallback message_callback,
             InteractionCallback interaction_callback,
             CommandCatalog command_catalog) {
    if (started_.exchange(true)) {
      throw std::logic_error{"Discord gateway may only be started once."};
    }

    message_callback_ = std::move(message_callback);
    interaction_callback_ = std::move(interaction_callback);
    command_catalog_ = std::move(command_catalog);
    command_catalog_version_.store(command_catalog_.version);
    log_handle_ =
        bot_.on_log([this, callbacks = callbacks_](const dpp::log_t &event) {
          invoke_callback(callbacks, [this, &event] {
            diagnostics_.emit({severity(event.severity),
                               "dpp",
                               "D++ emitted a redacted diagnostic event.",
                               {}});
          });
        });
    ready_handle_ =
        bot_.on_ready([this, callbacks = callbacks_](const dpp::ready_t &) {
          invoke_callback(callbacks, [this] {
            ready_.store(true);
            diagnostics_.emit(
                {DiagnosticSeverity::info,
                 "discord.ready",
                 "Sanguinius is connected as " + bot_.me.username + ".",
                 {}});
            synchronize_commands();
          });
        });
    accepting_.store(true);
    message_handle_ = bot_.on_message_create(
        [this, callbacks = callbacks_](const dpp::message_create_t &event) {
          invoke_callback(callbacks, [this, &event] {
            if (!accepting_.load()) {
              return;
            }
            try {
              message_callback_(incoming_message(event.msg, id(bot_.me.id)));
            } catch (const std::exception &error) {
              diagnostics_.emit({DiagnosticSeverity::error,
                                 "discord.message_translation",
                                 error.what(),
                                 {}});
            } catch (...) {
              diagnostics_.emit({DiagnosticSeverity::error,
                                 "discord.message_translation",
                                 "Unknown Discord message translation failure.",
                                 {}});
            }
          });
        });

    slash_handle_ = bot_.on_slashcommand(
        [this, callbacks = callbacks_](const dpp::slashcommand_t &event) {
          invoke_callback(callbacks, [this, &event, callbacks] {
            if (!accepting_.load()) {
              return;
            }
            try {
              auto interaction = incoming_interaction(
                  event, InteractionKind::slash_command, callbacks);
              const auto command = event.command.get_command_interaction();
              dpp_adapter_detail::translate_slash_command(command,
                                                          interaction);
              interaction_callback_(std::move(interaction));
            } catch (const std::exception &) {
              reject_interaction(event, "Slash-command translation failed.");
            } catch (...) {
              reject_interaction(event,
                                 "Unknown slash-command translation failure.");
            }
          });
        });
    button_handle_ = bot_.on_button_click(
        [this, callbacks = callbacks_](const dpp::button_click_t &event) {
          invoke_callback(callbacks, [this, &event, callbacks] {
            if (!accepting_.load()) {
              return;
            }
            try {
              auto interaction = incoming_interaction(
                  event, InteractionKind::button, callbacks);
              if (event.custom_id.size() > maximum_interaction_name_size) {
                throw std::invalid_argument{
                    "Discord component identifier exceeds safe bounds."};
              }
              interaction.custom_id = event.custom_id;
              interaction_callback_(std::move(interaction));
            } catch (const std::exception &) {
              reject_interaction(event, "Button translation failed.");
            } catch (...) {
              reject_interaction(event, "Unknown button translation failure.");
            }
          });
        });
    select_handle_ = bot_.on_select_click(
        [this, callbacks = callbacks_](const dpp::select_click_t &event) {
          invoke_callback(callbacks, [this, &event, callbacks] {
            if (!accepting_.load()) {
              return;
            }
            try {
              auto interaction = incoming_interaction(
                  event, InteractionKind::select_menu, callbacks);
              if (event.custom_id.size() > maximum_interaction_name_size ||
                  event.values.size() > maximum_select_values ||
                  std::any_of(event.values.begin(), event.values.end(),
                              [](const std::string &value) {
                                return value.size() > maximum_select_value_size;
                              })) {
                throw std::invalid_argument{
                    "Discord select interaction exceeds safe bounds."};
              }
              interaction.custom_id = event.custom_id;
              interaction.selected_values = event.values;
              interaction_callback_(std::move(interaction));
            } catch (const std::exception &) {
              reject_interaction(event, "Select-menu translation failed.");
            } catch (...) {
              reject_interaction(event,
                                 "Unknown select-menu translation failure.");
            }
          });
        });
    modal_handle_ = bot_.on_form_submit(
        [this, callbacks = callbacks_](const dpp::form_submit_t &event) {
          invoke_callback(callbacks, [this, &event, callbacks] {
            if (!accepting_.load()) {
              return;
            }
            try {
              auto interaction = incoming_interaction(
                  event, InteractionKind::modal_submit, callbacks);
              if (event.custom_id.size() > maximum_interaction_name_size) {
                throw std::invalid_argument{
                    "Discord modal identifier exceeds safe bounds."};
              }
              interaction.custom_id = event.custom_id;
              for (const auto &component : event.components) {
                append_modal_fields(component, interaction.modal_fields);
              }
              interaction_callback_(std::move(interaction));
            } catch (const std::exception &) {
              reject_interaction(event, "Modal translation failed.");
            } catch (...) {
              reject_interaction(event, "Unknown modal translation failure.");
            }
          });
        });
    message_context_handle_ = bot_.on_message_context_menu(
        [this,
         callbacks = callbacks_](const dpp::message_context_menu_t &event) {
          invoke_callback(callbacks, [this, &event, callbacks] {
            if (!accepting_.load()) {
              return;
            }
            try {
              auto interaction = incoming_interaction(
                  event, InteractionKind::message_context_command, callbacks);
              interaction.command_name = event.command.get_command_name();
              const auto &message = event.get_message();
              interaction.context_message =
                  dpp_adapter_detail::context_message_snapshot(message);
              interaction_callback_(std::move(interaction));
            } catch (const std::exception &) {
              reject_interaction(event, "Message-context translation failed.");
            } catch (...) {
              reject_interaction(
                  event, "Unknown message-context translation failure.");
            }
          });
        });

    cluster_host_->start();
  }

  void stop_accepting() noexcept {
    accepting_.store(false);
    ready_.store(false);
    try {
      if (message_handle_ != 0) {
        static_cast<void>(bot_.on_message_create.detach(message_handle_));
        message_handle_ = 0;
      }
      if (slash_handle_ != 0) {
        static_cast<void>(bot_.on_slashcommand.detach(slash_handle_));
        slash_handle_ = 0;
      }
      if (button_handle_ != 0) {
        static_cast<void>(bot_.on_button_click.detach(button_handle_));
        button_handle_ = 0;
      }
      if (select_handle_ != 0) {
        static_cast<void>(bot_.on_select_click.detach(select_handle_));
        select_handle_ = 0;
      }
      if (modal_handle_ != 0) {
        static_cast<void>(bot_.on_form_submit.detach(modal_handle_));
        modal_handle_ = 0;
      }
      if (message_context_handle_ != 0) {
        static_cast<void>(
            bot_.on_message_context_menu.detach(message_context_handle_));
        message_context_handle_ = 0;
      }
      if (ready_handle_ != 0) {
        static_cast<void>(bot_.on_ready.detach(ready_handle_));
        ready_handle_ = 0;
      }
    } catch (...) {
    }
  }

  void shutdown() noexcept {
    if (shutdown_.exchange(true)) {
      return;
    }
    stop_accepting();
    callbacks_->close_and_wait();
    try {
      cluster_host_->shutdown();
    } catch (const std::exception &error) {
      diagnostics_.emit(
          {DiagnosticSeverity::error, "discord.shutdown", error.what(), {}});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error,
                         "discord.shutdown",
                         "Unknown Discord shutdown failure.",
                         {}});
    }

    try {
      if (log_handle_ != 0) {
        static_cast<void>(bot_.on_log.detach(log_handle_));
        log_handle_ = 0;
      }
    } catch (...) {
    }
  }

  void synchronize_commands() {
    if (!registration_.begin()) {
      return;
    }
    const auto desired = dpp_adapter_detail::translate_command_catalog(
        command_catalog_, bot_.me.id);
    bot_.guild_commands_get(
        dpp::snowflake{guild_id_.value()},
        [this, callbacks = callbacks_,
         desired](const dpp::confirmation_callback_t &confirmation) {
          invoke_callback(callbacks, [this, callbacks, &confirmation, desired] {
            try {
              if (shutdown_.load() || !accepting_.load()) {
                registration_.cancel();
                return;
              }
              if (confirmation.is_error()) {
                registration_failed("Discord command catalog fetch failed.");
                return;
              }
              const auto existing = confirmation.get<dpp::slashcommand_map>();
              if (dpp_adapter_detail::commands_match(existing, desired)) {
                static_cast<void>(registration_.catalog_fetched(true, true));
                diagnostics_.emit({DiagnosticSeverity::info,
                                   "discord.commands",
                                   "Guild command catalog is synchronized.",
                                   {}});
                return;
              }
              if (registration_.catalog_fetched(true, false) !=
                  CommandCatalogFetchAction::update_required) {
                return;
              }
              bot_.guild_bulk_command_create(
                  desired, dpp::snowflake{guild_id_.value()},
                  [this,
                   callbacks](const dpp::confirmation_callback_t &updated) {
                    invoke_callback(callbacks, [this, &updated] {
                      if (shutdown_.load() || !accepting_.load()) {
                        registration_.cancel();
                        return;
                      }
                      if (updated.is_error()) {
                        registration_failed(
                            "Discord command catalog update failed.");
                        return;
                      }
                      registration_.catalog_updated(true);
                      diagnostics_.emit({DiagnosticSeverity::info,
                                         "discord.commands",
                                         "Guild command catalog was updated.",
                                         {}});
                    });
                  });
            } catch (const std::exception &) {
              registration_failed("Discord command catalog response failed.");
            } catch (...) {
              registration_failed("Discord command catalog response failed.");
            }
          });
        });
  }

  void registration_failed(const std::string_view message) noexcept {
    static_cast<void>(registration_.catalog_fetched(false, false));
    registration_.catalog_updated(false);
    diagnostics_.emit({DiagnosticSeverity::error,
                       "discord.commands",
                       std::string{message},
                       {}});
  }

  void reject_interaction(const dpp::interaction_create_t &event,
                          const std::string_view message) noexcept {
    diagnostics_.emit({DiagnosticSeverity::error,
                       "discord.interaction_translation",
                       std::string{message},
                       {}});
    try {
      auto responder =
          std::make_shared<DppInteractionResponder>(event, callbacks_);
      responder->reply(
          text_message("This interaction is malformed or unavailable."),
          ResponseVisibility::ephemeral, {});
    } catch (...) {
    }
  }

  [[nodiscard]] DiscordRuntimeStatus status() const noexcept {
    return DiscordRuntimeStatus{
        .ready = ready_.load(),
        .command_registration = registration_.state(),
        .command_catalog_version = command_catalog_version_.load(),
    };
  }

  std::shared_ptr<DppClusterHost> cluster_host_;
  dpp::cluster &bot_;
  Diagnostics &diagnostics_;
  std::chrono::seconds request_timeout_;
  DiscordId guild_id_;
  std::shared_ptr<CallbackFence> callbacks_;
  MessageCallback message_callback_;
  InteractionCallback interaction_callback_;
  CommandCatalog command_catalog_;
  std::atomic<bool> started_{false};
  std::atomic<bool> accepting_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> ready_{false};
  CommandRegistrationCoordinator registration_;
  std::atomic<std::uint32_t> command_catalog_version_{0};
  dpp::event_handle log_handle_{};
  dpp::event_handle ready_handle_{};
  dpp::event_handle message_handle_{};
  dpp::event_handle slash_handle_{};
  dpp::event_handle button_handle_{};
  dpp::event_handle select_handle_{};
  dpp::event_handle modal_handle_{};
  dpp::event_handle message_context_handle_{};
};

DppDiscordAdapter::DppDiscordAdapter(std::string token,
                                     const std::chrono::seconds request_timeout,
                                     const DiscordId guild_id,
                                     Diagnostics &diagnostics,
                                     const bool voice_enabled)
    : DppDiscordAdapter{
          std::make_shared<DppClusterHost>(std::move(token), voice_enabled),
          request_timeout, guild_id, diagnostics} {}

DppDiscordAdapter::DppDiscordAdapter(
    std::shared_ptr<DppClusterHost> cluster_host,
    const std::chrono::seconds request_timeout, const DiscordId guild_id,
    Diagnostics &diagnostics)
    : impl_{std::make_unique<Impl>(std::move(cluster_host), request_timeout,
                                   guild_id, diagnostics)} {}

DppDiscordAdapter::~DppDiscordAdapter() { shutdown(); }

void DppDiscordAdapter::start(MessageCallback message_callback,
                              InteractionCallback interaction_callback,
                              CommandCatalog command_catalog) {
  impl_->start(std::move(message_callback), std::move(interaction_callback),
               std::move(command_catalog));
}

void DppDiscordAdapter::stop_accepting() noexcept { impl_->stop_accepting(); }

void DppDiscordAdapter::shutdown() noexcept { impl_->shutdown(); }

void DppDiscordAdapter::show_typing(const DiscordId channel_id) {
  impl_->bot_.channel_typing(dpp::snowflake{channel_id.value()});
}

void DppDiscordAdapter::reply(const ReplyRequest &request) {
  dpp::message reply{dpp::snowflake{request.target.channel_id.value()},
                     request.content};
  if (request.embed.has_value()) {
    dpp::embed embed;
    embed.set_color(request.embed->color)
        .set_title(request.embed->title)
        .set_url(request.embed->url)
        .set_description(request.embed->description);
    reply.add_embed(embed);
  }
  reply.set_reference(dpp::snowflake{request.target.message_id.value()},
                      dpp::snowflake{request.target.guild_id.value()},
                      dpp::snowflake{request.target.channel_id.value()});
  if (request.suppress_mentions) {
    reply.set_allowed_mentions(false, false, false, false);
  }
  impl_->bot_.message_create(reply);
}

void DppDiscordAdapter::send_public(const PublicMessageRequest &request,
                                    const std::string_view provider_nonce,
                                    PublicDeliveryCallback callback) {
  std::string payload;
  try {
    payload = dpp_adapter_detail::durable_public_message_json(request,
                                                              provider_nonce);
  } catch (...) {
    if (callback) {
      callback({DeliveryResult::permanent_failure, std::nullopt});
    }
    return;
  }

  try {
    auto asynchronous_callback = callback;
    const auto base_path =
        dpp_adapter_detail::durable_public_message_base_path();
    dpp::rest_request<dpp::message>(
        &impl_->bot_, base_path.data(), request.channel_id.str(), "/messages",
        dpp::m_post, payload,
        [callbacks = impl_->callbacks_,
         callback = std::move(asynchronous_callback)](
            const dpp::confirmation_callback_t &confirmation) mutable {
          invoke_callback(callbacks, [&callback, &confirmation] {
            if (!callback) {
              return;
            }
            PublicDeliveryReceipt receipt{
                .result = delivery_result(confirmation),
                .provider_message_id = std::nullopt,
            };
            if (receipt.result == DeliveryResult::success) {
              try {
                receipt.provider_message_id =
                    dpp_adapter_detail::provider_message_id(
                        confirmation.get<dpp::message>());
              } catch (...) {
                receipt.result = DeliveryResult::unknown_outcome;
              }
            }
            callback(std::move(receipt));
          });
        });
  } catch (...) {
    if (callback) {
      callback({DeliveryResult::unknown_outcome, std::nullopt});
    }
  }
}

void DppDiscordAdapter::edit_public(const PublicMessageEditRequest &request,
                                    DeliveryCallback callback) {
  dpp::message message;
  try {
    if (!request.guild_id.is_set() || !request.channel_id.is_set() ||
        !request.message_id.is_set())
      throw std::invalid_argument{"Discord edit target is incomplete."};
    message = discord_message(request.message);
    message.id = dpp::snowflake{request.message_id.value()};
    message.channel_id = dpp::snowflake{request.channel_id.value()};
    message.guild_id = dpp::snowflake{request.guild_id.value()};
  } catch (...) {
    if (callback)
      callback(DeliveryResult::permanent_failure);
    return;
  }
  try {
    impl_->bot_.message_edit(
        message,
        [callbacks = impl_->callbacks_, callback = std::move(callback)](
            const dpp::confirmation_callback_t &confirmation) mutable {
          invoke_callback(callbacks, [&callback, &confirmation] {
            if (callback)
              callback(delivery_result(confirmation));
          });
        });
  } catch (...) {
    if (callback)
      callback(DeliveryResult::unknown_outcome);
  }
}

DiscordRuntimeStatus DppDiscordAdapter::status() const noexcept {
  return impl_->status();
}

std::vector<ConversationEntry>
DppDiscordAdapter::recent_messages(const IncomingMessage &message,
                                   const std::size_t limit,
                                   const std::stop_token stop_token) {
  auto result = std::make_shared<std::promise<dpp::message_map>>();
  auto future = result->get_future();
  impl_->bot_.messages_get(
      dpp::snowflake{message.channel_id.value()}, 0,
      dpp::snowflake{message.message_id.value()}, 0, limit,
      [result](const dpp::confirmation_callback_t &confirmation) {
        try {
          if (confirmation.is_error()) {
            throw std::runtime_error{confirmation.get_error().human_readable};
          }
          result->set_value(confirmation.get<dpp::message_map>());
        } catch (...) {
          try {
            result->set_exception(std::current_exception());
          } catch (...) {
          }
        }
      });

  auto message_map =
      wait_for_discord(future, stop_token, impl_->request_timeout_,
                       "Timed out retrieving recent Discord messages.");
  std::vector<ConversationEntry> messages;
  messages.reserve(message_map.size());
  for (auto &[message_id, recent] : message_map) {
    static_cast<void>(message_id);
    if (!recent.content.empty()) {
      messages.push_back(conversation_entry(recent));
    }
  }
  std::ranges::sort(messages, {}, &ConversationEntry::message_id);
  return messages;
}

ConversationEntry DppDiscordAdapter::message(const DiscordId message_id,
                                             const DiscordId channel_id,
                                             const std::stop_token stop_token) {
  auto result = std::make_shared<std::promise<dpp::message>>();
  auto future = result->get_future();
  impl_->bot_.message_get(
      dpp::snowflake{message_id.value()}, dpp::snowflake{channel_id.value()},
      [result](const dpp::confirmation_callback_t &confirmation) {
        try {
          if (confirmation.is_error()) {
            throw std::runtime_error{confirmation.get_error().human_readable};
          }
          result->set_value(confirmation.get<dpp::message>());
        } catch (...) {
          try {
            result->set_exception(std::current_exception());
          } catch (...) {
          }
        }
      });

  const auto fetched =
      wait_for_discord(future, stop_token, impl_->request_timeout_,
                       "Timed out retrieving the replied-to message.");
  return conversation_entry(fetched);
}

} // namespace sanguinius
