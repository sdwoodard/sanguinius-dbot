#include "sanguinius/pending_notice.hpp"

#include <chrono>
#include <stdexcept>

namespace sanguinius {
namespace {

constexpr auto test_notice_lifetime = std::chrono::hours{24};

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] InteractionTokenKind
interaction_token_kind(const InteractionKind kind) {
  switch (kind) {
  case InteractionKind::button:
    return InteractionTokenKind::button;
  case InteractionKind::select_menu:
    return InteractionTokenKind::select;
  case InteractionKind::modal_submit:
    return InteractionTokenKind::modal;
  case InteractionKind::slash_command:
  case InteractionKind::message_context_command:
    throw std::invalid_argument{"Interaction is not a component token kind."};
  }
  throw std::invalid_argument{"Interaction kind is invalid."};
}

} // namespace

PendingNoticeService::PendingNoticeService(PendingNoticeRepository &repository,
                                           const Clock &clock,
                                           PersistentIdGenerator &ids)
    : repository_{repository}, clock_{clock}, ids_{ids} {}

PendingNoticeCreation PendingNoticeService::create_test_notice(
    const IncomingInteraction &interaction) {
  const auto now_ms = unix_milliseconds(clock_);
  const auto lifetime_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          test_notice_lifetime)
          .count();
  auto persistence = repository_.create_with_token(CreatePendingNoticeRequest{
      .notice_id = ids_.next_id(),
      .token_id = ids_.next_id(),
      .target_user_id = interaction.user_id,
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .notice_type = "owner_test",
      .content = {"Sealed notice test", "The sealed-notice test succeeded."},
      .source_aggregate_type = "owner_test",
      .source_aggregate_id = interaction.interaction_id.str(),
      .expires_at_ms = now_ms + lifetime_ms,
      .notice_idempotency_key =
          "notice:create:test:" + interaction.interaction_id.str(),
      .token_idempotency_key =
          "token:create:test:" + interaction.interaction_id.str(),
      .created_at_ms = now_ms,
  });

  auto public_card = make_neutral_notice_card(CreatePendingNoticeRequest{
      .notice_id = persistence.notice.notice_id,
      .token_id = persistence.token_id,
      .target_user_id = interaction.user_id,
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .notice_type = "owner_test",
      .content = persistence.notice.content,
      .source_aggregate_type = "owner_test",
      .source_aggregate_id = interaction.interaction_id.str(),
      .expires_at_ms = now_ms + lifetime_ms,
      .notice_idempotency_key =
          "notice:create:test:" + interaction.interaction_id.str(),
      .token_idempotency_key =
          "token:create:test:" + interaction.interaction_id.str(),
      .created_at_ms = now_ms,
  });
  return {std::move(persistence), std::move(public_card)};
}

OpenPendingNoticeResult PendingNoticeService::open_component(
    const IncomingInteraction &interaction) const {
  const auto token_id = parse_component_token(interaction.custom_id);
  if (!token_id.has_value()) {
    return {.status = OpenPendingNoticeStatus::invalid_token,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  try {
    return repository_.open_by_token(OpenNoticeByTokenRequest{
        .token_id = *token_id,
        .interaction_kind = interaction_token_kind(interaction.kind),
        .guild_id = interaction.guild_id,
        .channel_id = interaction.channel_id,
        .user_id = interaction.user_id,
        .interaction_idempotency_key =
            "notice:component:" + interaction.interaction_id.str(),
        .now_ms = unix_milliseconds(clock_),
    });
  } catch (const std::invalid_argument &) {
    return {.status = OpenPendingNoticeStatus::wrong_kind,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
}

OpenPendingNoticeResult
PendingNoticeService::open_inbox(const IncomingInteraction &interaction) const {
  return repository_.open_next(OpenNextNoticeRequest{
      .user_id = interaction.user_id,
      .interaction_idempotency_key =
          "notice:inbox:" + interaction.interaction_id.str(),
      .now_ms = unix_milliseconds(clock_),
  });
}

PendingNoticeMutationStatus PendingNoticeService::complete_delivery(
    const std::string_view interaction_idempotency_key,
    const DeliveryResult result) const {
  if (result == DeliveryResult::success) {
    return repository_.confirm_open_delivery(
        std::string{interaction_idempotency_key}, unix_milliseconds(clock_));
  }
  return repository_.release_open_delivery(
      std::string{interaction_idempotency_key}, unix_milliseconds(clock_));
}

std::size_t PendingNoticeService::recover_incomplete_deliveries() const {
  return repository_.recover_incomplete_open_deliveries(
      unix_milliseconds(clock_));
}

std::size_t
PendingNoticeService::pending_count(const DiscordSnowflake &user_id) const {
  return repository_.pending_count(user_id, unix_milliseconds(clock_));
}

std::size_t PendingNoticeService::pending_count_all() const {
  return repository_.pending_count_all(unix_milliseconds(clock_));
}

std::optional<std::string>
parse_component_token(const std::string_view custom_id) {
  if (!custom_id.starts_with(component_token_prefix)) {
    return std::nullopt;
  }
  std::string token{custom_id.substr(component_token_prefix.size())};
  if (!valid_uuid_v4(token) ||
      custom_id.size() != component_token_prefix.size() + 36) {
    return std::nullopt;
  }
  return token;
}

std::string make_component_id(const std::string_view token_id) {
  const std::string token{token_id};
  if (!valid_uuid_v4(token)) {
    throw std::invalid_argument{"Component token must be a UUIDv4."};
  }
  return std::string{component_token_prefix} + token;
}

PublicMessageRequest
make_neutral_notice_card(const CreatePendingNoticeRequest &request) {
  return PublicMessageRequest{
      .guild_id = request.guild_id,
      .channel_id = request.channel_id,
      .message =
          InteractionMessage{
              .content = "<@" + request.target_user_id.str() +
                         ">, a sealed notice awaits.",
              .embed =
                  EmbedPayload{
                      .color = 0x8B0000U,
                      .title = "A sealed notice awaits",
                      .url = {},
                      .description =
                          "Status: pending. Only the addressed recipient may "
                          "open it. Expires in 24 hours.",
                  },
              .buttons = {ButtonPayload{
                  .custom_id = make_component_id(request.token_id),
                  .label = "Open sealed notice",
                  .disabled = false,
              }},
              .allowed_user_mentions = {request.target_user_id},
          },
  };
}

InteractionMessage
render_private_notice(const OpenPendingNoticeResult &result) {
  if (result.status == OpenPendingNoticeStatus::opened &&
      result.notice.has_value()) {
    InteractionMessage message = text_message(
        "**" + result.notice->content.title + "**\n" +
        result.notice->content.body);
    for (const auto &action : result.notice->content.actions) {
      message.buttons.push_back(ButtonPayload{.custom_id = action.custom_id,
                                              .label = action.label});
    }
    return message;
  }
  switch (result.status) {
  case OpenPendingNoticeStatus::no_pending_notice:
    return text_message("No sealed notices await you.");
  case OpenPendingNoticeStatus::wrong_user:
    return text_message("This sealed notice is not addressed to you.");
  case OpenPendingNoticeStatus::expired:
    return text_message("This sealed notice has expired.");
  case OpenPendingNoticeStatus::wrong_scope:
    return text_message("This control cannot be used here.");
  case OpenPendingNoticeStatus::wrong_kind:
  case OpenPendingNoticeStatus::invalid_token:
    return text_message("This control is invalid or no longer available.");
  case OpenPendingNoticeStatus::unavailable:
    return text_message("This sealed notice is no longer available.");
  case OpenPendingNoticeStatus::opened:
    break;
  }
  return text_message("This sealed notice could not be opened.");
}

} // namespace sanguinius
