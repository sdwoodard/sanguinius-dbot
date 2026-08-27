#include "sanguinius/presentation.hpp"

#include "sanguinius/command_registry.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sanguinius::presentation {
namespace {

constexpr std::string_view repository_url{
    "https://github.com/sdwoodard/sanguinius-dbot"};

[[nodiscard]] bool selected(const std::string_view topic,
                            const std::string_view command) {
  if (topic.empty() || topic == "all")
    return true;
  if (topic == "sanguinius")
    return command == "help" || command == "repo" || command == "sanguinius";
  return topic == command;
}

void add_chunks(EmbedPayload &embed, const std::string &heading,
                const std::vector<std::string> &lines) {
  std::string chunk;
  std::size_t part = 1;
  for (const auto &line : lines) {
    if (!chunk.empty() && chunk.size() + line.size() + 1 > 1'000) {
      embed.fields.push_back(
          {.name = part == 1 ? heading : heading + " · continued",
           .value = std::move(chunk)});
      chunk.clear();
      ++part;
    }
    if (!chunk.empty())
      chunk.push_back('\n');
    chunk += line;
  }
  if (!chunk.empty())
    embed.fields.push_back(
        {.name = part == 1 ? heading : heading + " · continued",
         .value = std::move(chunk)});
}

[[nodiscard]] std::string section_name(const std::string_view command) {
  if (command == "help" || command == "repo")
    return "Getting started";
  if (command == "sanguinius")
    return "Sanguinius & privacy";
  if (command == "chronicle")
    return "Living Chronicle";
  if (command == "tarot")
    return "Emperor's Tarot";
  if (command == "vox")
    return "Vox Sanguinius";
  return std::string{command};
}

} // namespace

InteractionMessage help(const std::string_view requested_topic,
                        const FeatureConfiguration &features,
                        const std::int64_t timestamp_ms) {
  if (timestamp_ms < 0)
    throw std::invalid_argument{"Presentation timestamp is invalid."};
  const auto topic =
      requested_topic.empty() ? std::string_view{"all"} : requested_topic;
  if (topic != "all" && topic != "sanguinius" && topic != "chronicle" &&
      topic != "tarot" && topic != "vox")
    throw std::invalid_argument{"Help topic is invalid."};
  const auto catalog =
      command_catalog(false, features.chronicle_enabled, features.tarot_enabled,
                      features.vox_enabled);
  EmbedPayload embed{
      .color = crimson,
      .title = "The halls of Sanguinius",
      .description =
          "Mention Sanguinius at the beginning of a message for a direct "
          "reply. Slash-command responses are private unless their "
          "description says otherwise.",
      .footer = "Member-facing commands only",
      .timestamp_ms = timestamp_ms,
  };
  std::string current_section;
  std::vector<std::string> lines;
  const auto flush = [&] {
    if (!lines.empty()) {
      add_chunks(embed, current_section, lines);
      lines.clear();
    }
  };
  for (const auto &command : catalog.commands) {
    if (command.kind != ApplicationCommandKind::chat_input ||
        command.audience != CommandAudience::member ||
        !selected(topic, command.name))
      continue;
    const auto section = section_name(command.name);
    if (!current_section.empty() && section != current_section)
      flush();
    current_section = section;
    if (command.subcommands.empty() && command.subcommand_groups.empty()) {
      lines.push_back("`/" + command.name + "` — " + command.description);
      continue;
    }
    for (const auto &subcommand : command.subcommands) {
      if (subcommand.audience == CommandAudience::member)
        lines.push_back("`/" + command.name + " " + subcommand.name + "` — " +
                        subcommand.description);
    }
    for (const auto &group : command.subcommand_groups) {
      for (const auto &subcommand : group.subcommands) {
        if (subcommand.audience == CommandAudience::member)
          lines.push_back("`/" + command.name + " " + group.name + " " +
                          subcommand.name + "` — " + subcommand.description);
      }
    }
  }
  flush();
  const bool unavailable =
      (topic == "chronicle" && !features.chronicle_enabled) ||
      (topic == "tarot" && !features.tarot_enabled) ||
      (topic == "vox" && !features.vox_enabled);
  if (unavailable)
    embed.fields.push_back(
        {.name = section_name(topic),
         .value = "This command family is currently unavailable."});
  if ((topic == "all" || topic == "chronicle") && features.chronicle_enabled)
    embed.fields.push_back(
        {.name = "Message action",
         .value =
             "`Canonize in the Chronicle` — open a private canon preview."});
  InteractionMessage message{.content = {},
                             .embed = std::move(embed),
                             .buttons = {},
                             .allowed_user_mentions = {}};
  validate(message);
  return message;
}

InteractionMessage repository(const std::int64_t timestamp_ms) {
  if (timestamp_ms < 0)
    throw std::invalid_argument{"Presentation timestamp is invalid."};
  InteractionMessage message{
      .content = {},
      .embed =
          EmbedPayload{
              .color = crimson,
              .title = "Sanguinius source code",
              .url = std::string{repository_url},
              .description = "Build instructions, source code, and the public "
                             "project history.",
              .footer =
                  "The source is public; server data and credentials are not.",
              .timestamp_ms = timestamp_ms,
          },
      .buttons = {},
      .allowed_user_mentions = {},
  };
  validate(message);
  return message;
}

InteractionMessage error(const ErrorKind kind,
                         const std::string_view retry_command) {
  std::string content;
  switch (kind) {
  case ErrorKind::wrong_scope:
    content = "Use this interaction in the configured primary server channel.";
    break;
  case ErrorKind::forbidden:
    content = "That is an owner-only control and is not available to you.";
    break;
  case ErrorKind::malformed:
    content = "That request is malformed or invalid. Please run the slash "
              "command again.";
    break;
  case ErrorKind::stale:
    content =
        "That control is stale, invalid, or no longer matches current state.";
    break;
  case ErrorKind::expired:
    content = "That control has expired.";
    break;
  case ErrorKind::busy:
    content = "Sanguinius could not complete that interaction because it is "
              "handling too many interactions right now. Please try again "
              "shortly.";
    break;
  case ErrorKind::feature_disabled:
    content = "That feature is currently disabled or not available yet.";
    break;
  case ErrorKind::provider_degraded:
    content = "The prose provider is temporarily unavailable; deterministic "
              "commands remain ready.";
    break;
  case ErrorKind::duplicate:
    content = "That interaction was already handled.";
    break;
  }
  if (!retry_command.empty())
    content += " Run `" + std::string{retry_command} + "` for a fresh view.";
  return text_message(std::move(content));
}

ButtonStyle action_button_style(const std::string_view label) noexcept {
  if (label == "Retract" || label.starts_with("Retract ") ||
      label == "Discard" || label == "Cancel" || label == "Consent to void")
    return ButtonStyle::danger;
  if (label == "Approve" || label == "Submit" || label == "Confirm" ||
      label == "Open sealed notice" || label == "Open wager form" ||
      label == "Confirm offer" || label == "Accept and fund" ||
      label == "Submit outcome")
    return ButtonStyle::primary;
  return ButtonStyle::secondary;
}

void validate(const InteractionMessage &message) {
  if (message.content.size() > 2'000 || message.buttons.size() > 5 ||
      message.allowed_user_mentions.size() > 1)
    throw std::invalid_argument{
        "Interaction presentation exceeds safe bounds."};
  std::unordered_set<std::string_view> custom_ids;
  for (const auto &button : message.buttons) {
    if (button.custom_id.empty() || button.custom_id.size() > 100 ||
        button.label.empty() || button.label.size() > 80 ||
        !custom_ids.insert(button.custom_id).second)
      throw std::invalid_argument{"Interaction button is invalid."};
  }
  if (!message.embed)
    return;
  std::size_t total = message.embed->title.size() +
                      message.embed->description.size() +
                      message.embed->footer.size();
  if (message.embed->title.size() > 256 ||
      message.embed->description.size() > 4'096 ||
      message.embed->footer.size() > 2'048 || message.embed->fields.size() > 25)
    throw std::invalid_argument{"Embed presentation exceeds safe bounds."};
  if (message.embed->timestamp_ms && *message.embed->timestamp_ms < 0)
    throw std::invalid_argument{"Embed timestamp is invalid."};
  for (const auto &field : message.embed->fields) {
    total += field.name.size() + field.value.size();
    if (field.name.empty() || field.name.size() > 256 || field.value.empty() ||
        field.value.size() > 1'024)
      throw std::invalid_argument{"Embed field exceeds safe bounds."};
  }
  if (total > 6'000)
    throw std::invalid_argument{"Embed presentation exceeds total bounds."};
}

} // namespace sanguinius::presentation
