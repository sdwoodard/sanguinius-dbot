#pragma once

#include "sanguinius/snowflake.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sanguinius {

struct MessageReference {
  DiscordId message_id{};
  DiscordId guild_id{};
  DiscordId channel_id{};
};

struct ContextUserSnapshot {
  DiscordId user_id{};
  std::string username;
  std::string display_name;
  bool is_bot{};
};

struct ContextAttachmentSnapshot {
  DiscordId attachment_id{};
  std::string filename;
  std::optional<std::string> content_type{};
  std::uint64_t byte_size{};
  std::optional<std::uint32_t> width{};
  std::optional<std::uint32_t> height{};
  bool ephemeral{};
  bool spoiler{};
};

struct ContextMessageSnapshot {
  MessageReference reference;
  ContextUserSnapshot author;
  std::string content;
  bool content_truncated{};
  std::int64_t occurred_at_ms{};
  std::vector<ContextUserSnapshot> mentioned_users{};
  std::vector<ContextAttachmentSnapshot> attachments{};
};

struct IncomingMessage {
  std::string correlation_id;
  DiscordId bot_user_id{};
  DiscordId message_id{};
  DiscordId guild_id{};
  DiscordId channel_id{};
  DiscordId author_user_id{};
  std::string author_username;
  std::string author_display_name;
  std::string content;
  bool author_is_bot{};
  std::optional<MessageReference> replied_to;
};

struct ConversationEntry {
  DiscordId message_id{};
  std::string author_username;
  std::string author_display_name;
  std::string content;
};

struct EmbedPayload {
  std::uint32_t color{};
  std::string title;
  std::string url{};
  std::string description;
};

struct ReplyRequest {
  MessageReference target;
  std::string content;
  std::optional<EmbedPayload> embed;
  bool suppress_mentions{true};
};

enum class ApplicationCommandKind {
  chat_input,
  message_context,
};

enum class CommandOptionKind {
  string,
  user,
};

struct CommandOptionChoiceDefinition {
  std::string name;
  std::string value;

  [[nodiscard]] bool
  operator==(const CommandOptionChoiceDefinition &) const = default;
};

struct CommandOptionDefinition {
  CommandOptionKind kind{CommandOptionKind::string};
  std::string name;
  std::string description;
  bool required{};
  std::size_t minimum_length{};
  std::size_t maximum_length{};
  std::vector<CommandOptionChoiceDefinition> choices{};

  [[nodiscard]] bool operator==(const CommandOptionDefinition &) const =
      default;
};

struct CommandSubcommandDefinition {
  std::string name;
  std::string description;
  std::vector<CommandOptionDefinition> options{};

  [[nodiscard]] bool
  operator==(const CommandSubcommandDefinition &) const = default;
};

struct CommandSubcommandGroupDefinition {
  std::string name;
  std::string description;
  std::vector<CommandSubcommandDefinition> subcommands{};

  [[nodiscard]] bool
  operator==(const CommandSubcommandGroupDefinition &) const = default;
};

struct CommandDefinition {
  std::string name;
  std::string description;
  std::vector<CommandSubcommandDefinition> subcommands;
  ApplicationCommandKind kind{ApplicationCommandKind::chat_input};
  std::vector<CommandSubcommandGroupDefinition> subcommand_groups{};

  [[nodiscard]] bool operator==(const CommandDefinition &) const = default;
};

struct CommandCatalog {
  std::uint32_t version{};
  std::vector<CommandDefinition> commands;

  [[nodiscard]] bool operator==(const CommandCatalog &) const = default;
};

enum class InteractionKind {
  slash_command,
  button,
  select_menu,
  modal_submit,
  message_context_command,
};

enum class ResponseVisibility {
  public_message,
  ephemeral,
};

enum class DeliveryResult {
  success,
  transient_failure,
  unknown_outcome,
  permanent_failure,
};

enum class CommandRegistrationState {
  not_started,
  synchronizing,
  synchronized,
  failed,
};

inline constexpr std::size_t maximum_interaction_options = 25;
inline constexpr std::size_t maximum_select_values = 25;
inline constexpr std::size_t maximum_modal_fields = 5;
inline constexpr std::size_t maximum_interaction_name_size = 100;
inline constexpr std::size_t maximum_select_value_size = 100;
inline constexpr std::size_t maximum_modal_value_size = 4'000;

using InteractionOptionValue =
    std::variant<std::string, std::int64_t, bool, DiscordId, double>;

struct InteractionOption {
  std::string name;
  InteractionOptionValue value;
};

struct DiscordRuntimeStatus {
  bool ready{};
  CommandRegistrationState command_registration{
      CommandRegistrationState::not_started};
  std::uint32_t command_catalog_version{};
};

enum class ButtonStyle {
  primary,
  secondary,
};

struct ButtonPayload {
  std::string custom_id;
  std::string label;
  bool disabled{};
  ButtonStyle style{ButtonStyle::primary};
};

struct InteractionMessage {
  std::string content;
  std::optional<EmbedPayload> embed;
  std::vector<ButtonPayload> buttons;
  std::vector<DiscordId> allowed_user_mentions;
};

[[nodiscard]] inline InteractionMessage text_message(std::string content) {
  return InteractionMessage{std::move(content), std::nullopt, {}, {}};
}

struct ModalFieldPayload {
  enum class Style {
    short_text,
    paragraph,
  };

  std::string custom_id;
  std::string label;
  std::string value{};
  std::size_t minimum_length{};
  std::size_t maximum_length{};
  bool required{true};
  Style style{Style::short_text};
};

struct ModalPayload {
  std::string custom_id;
  std::string title;
  std::vector<ModalFieldPayload> fields;
};

struct PublicMessageRequest {
  DiscordId guild_id{};
  DiscordId channel_id{};
  InteractionMessage message;
};

struct PublicDeliveryReceipt {
  DeliveryResult result{DeliveryResult::permanent_failure};
  std::optional<DiscordId> provider_message_id;
};

class DiscordInteractionResponder;

struct IncomingInteraction {
  std::string correlation_id;
  DiscordId interaction_id{};
  DiscordId guild_id{};
  DiscordId channel_id{};
  DiscordId user_id{};
  std::string username;
  std::string display_name;
  InteractionKind kind{InteractionKind::slash_command};
  std::string command_name;
  std::string subcommand_group_name;
  std::string subcommand_name;
  std::vector<InteractionOption> command_options;
  std::string custom_id;
  std::vector<std::string> selected_values;
  std::vector<std::pair<std::string, std::string>> modal_fields;
  std::optional<ContextMessageSnapshot> context_message;
  std::shared_ptr<DiscordInteractionResponder> responder;
};

} // namespace sanguinius
