#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/repositories.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace sanguinius {

inline constexpr std::string_view component_token_prefix{"sg:1:"};

struct PendingNoticeCreation {
  CreatePendingNoticeResult persistence;
  PublicMessageRequest public_card;
};

class PendingNoticeService {
public:
  PendingNoticeService(PendingNoticeRepository &repository, const Clock &clock,
                       PersistentIdGenerator &ids);

  [[nodiscard]] PendingNoticeCreation
  create_test_notice(const IncomingInteraction &interaction);
  [[nodiscard]] OpenPendingNoticeResult
  open_component(const IncomingInteraction &interaction) const;
  [[nodiscard]] OpenPendingNoticeResult
  open_inbox(const IncomingInteraction &interaction) const;
  [[nodiscard]] PendingNoticeMutationStatus
  complete_delivery(std::string_view interaction_idempotency_key,
                    DeliveryResult result) const;
  [[nodiscard]] std::size_t recover_incomplete_deliveries() const;
  [[nodiscard]] std::size_t
  pending_count(const DiscordSnowflake &user_id) const;
  [[nodiscard]] std::size_t pending_count_all() const;

private:
  PendingNoticeRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
};

[[nodiscard]] std::optional<std::string>
parse_component_token(std::string_view custom_id);
[[nodiscard]] std::string make_component_id(std::string_view token_id);
[[nodiscard]] PublicMessageRequest
make_neutral_notice_card(const CreatePendingNoticeRequest &request);
[[nodiscard]] InteractionMessage
render_private_notice(const OpenPendingNoticeResult &result);

} // namespace sanguinius
