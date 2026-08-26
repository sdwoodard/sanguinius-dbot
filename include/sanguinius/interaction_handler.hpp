#pragma once

#include "sanguinius/appearances.hpp"
#include "sanguinius/callback_fence.hpp"
#include "sanguinius/chronicle.hpp"
#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/durable_work_controls.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/pending_notice.hpp"
#include "sanguinius/relationships.hpp"
#include "sanguinius/repositories.hpp"
#include "sanguinius/tarot.hpp"
#include "sanguinius/tarot_house.hpp"
#include "sanguinius/tarot_integration.hpp"
#include "sanguinius/wagers.hpp"
#include "sanguinius/vox.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace sanguinius {

enum class InteractionOperation {
  status,
  inbox,
  privacy,
  appearance_callbacks,
  admin_health,
  work_recent,
  work_dead,
  test_notice,
  test_schedule_notice,
  test_public_retry,
  open_component,
  chronicle_canonize,
  chronicle_memory_preview,
  chronicle_recall,
  chronicle_search_component,
  chronicle_timeline,
  chronicle_forget,
  chronicle_profile,
  chronicle_callbacks,
  chronicle_edit,
  chronicle_component,
  chronicle_session_start,
  chronicle_session_status,
  chronicle_session_close,
  chronicle_summary_edit,
  chronicle_summary_approve,
  chronicle_summary_reject,
  chronicle_summary_component,
  chronicle_title_propose,
  chronicle_title_list,
  chronicle_title_approve,
  chronicle_title_reject,
  chronicle_title_feature,
  chronicle_title_revoke,
  chronicle_anniversaries_on,
  chronicle_anniversaries_off,
  test_anniversary,
  appearance_simulate,
  appearance_preview,
  appearance_recent,
  appearance_quiet_for,
  appearance_quiet_tonight,
  appearance_quiet_until,
  appearance_quiet_off,
  appearance_feedback,
  appearance_feedback_component,
  appearance_trigger,
  appearance_disable,
  appearance_enable,
  tarot_balance,
  tarot_history,
  tarot_standings,
  tarot_standings_visibility,
  tarot_grace,
  tarot_trial,
  tarot_draw,
  tarot_record,
  tarot_house_offers,
  tarot_house_play,
  tarot_house_history,
  tarot_component,
  tarot_house_component,
  tarot_adjust,
  tarot_reverse,
  tarot_economy,
  tarot_draw_test,
  tarot_draw_replay,
  tarot_house_offer,
  tarot_house_resolve,
  tarot_house_deadline,
  tarot_house_cleanup,
  tarot_integration_preview,
  tarot_integration_retry,
  wager_create,
  wager_preview,
  wager_component,
  wager_action,
  wager_outcome,
  wager_evidence,
  wager_judgment,
  wager_history,
  wager_disputes,
  wager_test_role,
  wager_test_deadline,
  wager_test_cleanup,
  vox_summon,
  vox_status,
  vox_leave,
  vox_say,
  vox_mute,
  vox_voice,
  vox_test_disconnect,
  vox_speech_test,
};

struct RoutedInteraction {
  IncomingInteraction interaction;
  InteractionOperation operation{InteractionOperation::status};
};

class InteractionHandler {
public:
  InteractionHandler(
      CoreIdentityRepository &identities, PendingNoticeService &notices,
      const Clock &clock, DurableWorkControlService &durable_controls,
      ChronicleService *chronicle, ChronicleSessionService *chronicle_sessions,
      HealthService &health_service, Diagnostics &diagnostics,
      FeatureConfiguration features,
      std::function<QueueSnapshot()> message_queue,
      std::function<QueueSnapshot()> ai_queue, std::size_t queue_capacity = 64,
      RelationshipService *relationships = nullptr,
      AppearanceService *appearances = nullptr, TarotService *tarot = nullptr,
      TarotWagerService *wagers = nullptr,
      TarotDrawService *tarot_draws = nullptr,
      TarotHouseService *tarot_house = nullptr,
      TarotIntegrationService *tarot_integration = nullptr,
      VoxService *vox = nullptr);
  ~InteractionHandler();

  InteractionHandler(const InteractionHandler &) = delete;
  InteractionHandler &operator=(const InteractionHandler &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SubmitResult enqueue(RoutedInteraction interaction);
  [[nodiscard]] QueueSnapshot queue_snapshot() const;

private:
  void process(const RoutedInteraction &request);
  void ensure_user(const IncomingInteraction &interaction);
  void edit(const IncomingInteraction &interaction, InteractionMessage message,
            std::string_view diagnostic_category) const noexcept;
  void edit_reveal(const IncomingInteraction &interaction,
                   OpenPendingNoticeResult reveal,
                   std::string_view diagnostic_category) const noexcept;

  CoreIdentityRepository &identities_;
  PendingNoticeService &notices_;
  const Clock &clock_;
  DurableWorkControlService &durable_controls_;
  ChronicleService *chronicle_{};
  ChronicleSessionService *chronicle_sessions_{};
  RelationshipService *relationships_{};
  AppearanceService *appearances_{};
  TarotService *tarot_{};
  TarotWagerService *wagers_{};
  TarotDrawService *tarot_draws_{};
  TarotHouseService *tarot_house_{};
  TarotIntegrationService *tarot_integration_{};
  VoxService *vox_{};
  HealthService &health_service_;
  Diagnostics &diagnostics_;
  FeatureConfiguration features_;
  std::function<QueueSnapshot()> message_queue_;
  std::function<QueueSnapshot()> ai_queue_;
  std::shared_ptr<CallbackFence> callbacks_;
  BoundedExecutor worker_;
};

} // namespace sanguinius
