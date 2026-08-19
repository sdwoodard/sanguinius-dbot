#include "sanguinius/interaction_handler.hpp"

#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::optional<std::string> cache_value(const std::string &value) {
  if (value.empty() || value.size() > 128) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::string enabled(const bool value) {
  return value ? "enabled" : "disabled";
}

[[nodiscard]] InteractionMessage
status_message(const FeatureConfiguration &features,
               const std::size_t pending_notices) {
  std::ostringstream output;
  output << "Sanguinius is ready.\n"
         << "Unopened sealed notices: " << pending_notices << "\n"
         << "Chronicle: " << enabled(features.chronicle_enabled) << "\n"
         << "Tarot: " << enabled(features.tarot_enabled) << "\n"
         << "Appearances: " << appearance_mode_name(features.appearances_mode)
         << "\n"
         << "Vox: " << enabled(features.vox_enabled);
  return text_message(output.str());
}

[[nodiscard]] InteractionMessage
privacy_message(const FeatureConfiguration &features,
                const UserPreferences &preferences,
                const std::size_t pending_notices) {
  std::ostringstream output;
  output << "Your Sanguinius privacy summary\n"
         << "Discord identity cache: stored for stable account identity\n"
         << "Chronicle opt-in: " << enabled(preferences.chronicle_opt_in)
         << "\nMemory callbacks: "
         << enabled(preferences.memory_callback_opt_in)
         << "\nChronicle anniversaries: "
         << enabled(preferences.chronicle_opt_in &&
                    preferences.anniversary_reminders_enabled)
         << "\nAppearance callbacks: "
         << enabled(preferences.appearance_callback_opt_in)
         << "\nVoice input globally: " << enabled(features.voice_input_enabled)
         << "\nYour voice-input opt-in: "
         << enabled(preferences.voice_input_opt_in)
         << "\nUnopened sealed notices: " << pending_notices
         << "\nDiscord DMs are never used. Raw received voice audio is never "
            "persisted.";
  return text_message(output.str());
}

} // namespace

InteractionHandler::InteractionHandler(
    CoreIdentityRepository &identities, PendingNoticeService &notices,
    const Clock &clock, DurableWorkControlService &durable_controls,
    ChronicleService *chronicle,
    ChronicleSessionService *chronicle_sessions,
    HealthService &health_service, Diagnostics &diagnostics,
    const FeatureConfiguration features,
    std::function<QueueSnapshot()> message_queue,
    std::function<QueueSnapshot()> ai_queue, const std::size_t queue_capacity,
    RelationshipService *relationships)
    : identities_{identities}, notices_{notices}, clock_{clock},
      durable_controls_{durable_controls}, chronicle_{chronicle},
      chronicle_sessions_{chronicle_sessions},
      relationships_{relationships},
      health_service_{health_service},
      diagnostics_{diagnostics}, features_{features},
      message_queue_{std::move(message_queue)}, ai_queue_{std::move(ai_queue)},
      callbacks_{std::make_shared<CallbackFence>()},
      worker_{queue_capacity, 1} {
  if (!message_queue_ || !ai_queue_) {
    throw std::invalid_argument{"Interaction queue observers are required."};
  }
}

InteractionHandler::~InteractionHandler() { stop(); }

void InteractionHandler::start() { worker_.start(); }

void InteractionHandler::stop() noexcept {
  worker_.stop();
  callbacks_->close_and_wait();
}

SubmitResult InteractionHandler::enqueue(RoutedInteraction interaction) {
  auto responder = interaction.interaction.responder;
  auto correlation_id = interaction.interaction.correlation_id;
  const auto result = worker_.try_submit([this,
                                          interaction = std::move(interaction)](
                                             const std::stop_token token) {
    if (token.stop_requested()) {
      return;
    }
    try {
      process(interaction);
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "interaction.handling",
                         error.what(), interaction.interaction.correlation_id});
      edit(interaction.interaction,
           text_message("I could not complete that interaction. Please "
                        "try again shortly."),
           "interaction.response");
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error, "interaction.handling",
                         "Unknown interaction handling failure.",
                         interaction.interaction.correlation_id});
      edit(interaction.interaction,
           text_message("I could not complete that interaction. Please "
                        "try again shortly."),
           "interaction.response");
    }
  });
  if (result == SubmitResult::full) {
    diagnostics_.emit({DiagnosticSeverity::warning, "interaction.queue_full",
                       "Incoming interaction was rejected because the "
                       "interaction queue is full.",
                       correlation_id});
    if (responder) {
      responder->edit_original(text_message(
          "I am handling too many interactions right now. Please try again "
          "shortly."));
    }
  }
  return result;
}

QueueSnapshot InteractionHandler::queue_snapshot() const {
  return worker_.snapshot();
}

void InteractionHandler::process(const RoutedInteraction &request) {
  ensure_user(request.interaction);
  switch (request.operation) {
  case InteractionOperation::status:
    edit(request.interaction,
         status_message(features_,
                        notices_.pending_count(request.interaction.user_id)),
         "interaction.status");
    return;
  case InteractionOperation::privacy: {
    const auto preferences =
        identities_.load_preferences(request.interaction.user_id);
    if (!preferences.has_value()) {
      throw std::runtime_error{"User preferences were not initialized."};
    }
    edit(request.interaction,
         privacy_message(features_, *preferences,
                         notices_.pending_count(request.interaction.user_id)),
         "interaction.privacy");
    return;
  }
  case InteractionOperation::inbox:
    edit_reveal(request.interaction, notices_.open_inbox(request.interaction),
                "interaction.inbox");
    return;
  case InteractionOperation::open_component:
    edit_reveal(request.interaction,
                notices_.open_component(request.interaction),
                "interaction.component");
    return;
  case InteractionOperation::chronicle_canonize:
    if (!chronicle_) throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         render_chronicle_proposal(chronicle_->canonize_message(request.interaction)),
         "interaction.chronicle_canonize");
    return;
  case InteractionOperation::chronicle_memory_preview:
    if (!chronicle_) throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction, chronicle_->begin_memory_preview(request.interaction),
         "interaction.chronicle_memory_preview");
    return;
  case InteractionOperation::chronicle_recall:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle search service is unavailable."};
    edit(request.interaction, chronicle_sessions_->search(request.interaction),
         "interaction.chronicle_recall");
    return;
  case InteractionOperation::chronicle_search_component:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle search service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->advance_search(request.interaction),
         "interaction.chronicle_search_page");
    return;
  case InteractionOperation::chronicle_timeline:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle timeline service is unavailable."};
    edit(request.interaction, chronicle_sessions_->timeline(request.interaction),
         "interaction.chronicle_timeline");
    return;
  case InteractionOperation::chronicle_forget:
    if (!chronicle_) throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction, chronicle_->forget(request.interaction),
         "interaction.chronicle_forget");
    return;
  case InteractionOperation::chronicle_profile:
    if (!relationships_)
      throw std::runtime_error{"Relationship service is unavailable."};
    edit(request.interaction, relationships_->profile(request.interaction),
         "interaction.chronicle_profile");
    return;
  case InteractionOperation::chronicle_callbacks:
    if (!relationships_)
      throw std::runtime_error{"Relationship service is unavailable."};
    edit(request.interaction,
         relationships_->set_memory_callbacks(request.interaction),
         "interaction.chronicle_callbacks");
    return;
  case InteractionOperation::chronicle_edit:
    if (!chronicle_) throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         render_chronicle_mutation(chronicle_->edit_proposal(request.interaction)),
         "interaction.chronicle_edit");
    return;
  case InteractionOperation::chronicle_component:
    if (!chronicle_) throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         render_chronicle_mutation(chronicle_->apply_component(request.interaction)),
         "interaction.chronicle_component");
    return;
  case InteractionOperation::chronicle_session_start: {
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    const auto result = chronicle_sessions_->start(request.interaction);
    edit(request.interaction,
         text_message(result.code == ChronicleSessionResultCode::created
                          ? "The Chronicle session is now open."
                          : result.code == ChronicleSessionResultCode::existing
                                ? "A Chronicle session is already active."
                                : result.code == ChronicleSessionResultCode::opted_out
                                      ? "Enable Chronicle participation before starting a session."
                                      : "The Chronicle session could not be opened."),
         "interaction.chronicle_session_start");
    return;
  }
  case InteractionOperation::chronicle_session_status:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction, chronicle_sessions_->status(request.interaction),
         "interaction.chronicle_session_status");
    return;
  case InteractionOperation::chronicle_session_close: {
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    const auto result = chronicle_sessions_->close(request.interaction);
    edit(request.interaction,
         text_message(result.code == ChronicleSessionResultCode::updated
                          ? (result.wake_scheduler
                                 ? "The session is closed; its chapter draft is being prepared."
                                 : "The empty session was abandoned without a chapter.")
                          : result.code == ChronicleSessionResultCode::unauthorized
                                ? "Only the opener or owner may close this session."
                                : "The Chronicle session could not be closed."),
         "interaction.chronicle_session_close");
    return;
  }
  case InteractionOperation::chronicle_summary_edit:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction, chronicle_sessions_->edit_summary(request.interaction),
         "interaction.chronicle_summary_edit");
    return;
  case InteractionOperation::chronicle_summary_approve:
  case InteractionOperation::chronicle_summary_reject:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->decide_summary(
             request.interaction,
             request.operation == InteractionOperation::chronicle_summary_approve),
         "interaction.chronicle_summary_decision");
    return;
  case InteractionOperation::chronicle_summary_component:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->apply_summary_control(request.interaction),
         "interaction.chronicle_summary_component");
    return;
  case InteractionOperation::chronicle_title_propose:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction, chronicle_sessions_->propose_title(request.interaction),
         "interaction.chronicle_title_propose");
    return;
  case InteractionOperation::chronicle_title_list:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction, chronicle_sessions_->list_titles(request.interaction),
         "interaction.chronicle_title_list");
    return;
  case InteractionOperation::chronicle_title_approve:
  case InteractionOperation::chronicle_title_reject:
  case InteractionOperation::chronicle_title_feature:
  case InteractionOperation::chronicle_title_revoke: {
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    const auto action =
        request.operation == InteractionOperation::chronicle_title_approve
            ? TitleAction::approve
        : request.operation == InteractionOperation::chronicle_title_reject
            ? TitleAction::reject
        : request.operation == InteractionOperation::chronicle_title_feature
            ? TitleAction::feature
            : TitleAction::revoke;
    edit(request.interaction,
         chronicle_sessions_->mutate_title(request.interaction, action),
         "interaction.chronicle_title_mutation");
    return;
  }
  case InteractionOperation::chronicle_anniversaries_on:
  case InteractionOperation::chronicle_anniversaries_off:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->set_anniversaries(
             request.interaction,
             request.operation == InteractionOperation::chronicle_anniversaries_on),
         "interaction.chronicle_anniversary_preference");
    return;
  case InteractionOperation::test_anniversary:
    if (!chronicle_sessions_) throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         text_message(chronicle_sessions_->queue_test_anniversary(request.interaction)
                          ? "A test anniversary scan is queued."
                          : "That test anniversary scan was already queued or is unavailable."),
         "interaction.test_anniversary");
    return;
  case InteractionOperation::admin_health: {
    const auto snapshot =
        health_service_.snapshot(message_queue_(), ai_queue_(), true);
    edit(request.interaction, text_message(render_health(snapshot)),
         "interaction.health");
    return;
  }
  case InteractionOperation::work_recent:
    edit(request.interaction,
         text_message(render_work_inspection(durable_controls_.recent(),
                                             "Recent durable work")),
         "interaction.work_recent");
    return;
  case InteractionOperation::work_dead:
    edit(request.interaction,
         text_message(render_work_inspection(durable_controls_.dead(),
                                             "Failed and dead durable work")),
         "interaction.work_dead");
    return;
  case InteractionOperation::test_notice: {
    const auto created =
        durable_controls_.queue_test_notice(request.interaction);
    edit(request.interaction,
         text_message(created ? "The private test notice is queued for durable "
                                "delivery."
                              : "That private test notice was already queued."),
         "interaction.test_notice");
    return;
  }
  case InteractionOperation::test_schedule_notice: {
    const auto created =
        durable_controls_.schedule_test_notice(request.interaction);
    edit(request.interaction,
         text_message(created
                          ? "A private test notice is scheduled for 60 seconds "
                            "from now."
                          : "That private test notice was already scheduled."),
         "interaction.test_schedule_notice");
    return;
  }
  case InteractionOperation::test_public_retry: {
    const auto created =
        durable_controls_.queue_test_public_retry(request.interaction);
    edit(request.interaction,
         text_message(created
                          ? "The synthetic public retry is queued. Its first "
                            "attempt will fail before Discord submission."
                          : "That synthetic public retry was already queued."),
         "interaction.test_public_retry");
    return;
  }
  }
}

void InteractionHandler::ensure_user(const IncomingInteraction &interaction) {
  identities_.ensure_user(DiscordUserRecord{
      .user_id = interaction.user_id,
      .display_name = cache_value(interaction.display_name),
      .username = cache_value(interaction.username),
      .is_bot = false,
      .observed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            clock_.now().time_since_epoch())
                            .count(),
  });
}

void InteractionHandler::edit(
    const IncomingInteraction &interaction, InteractionMessage message,
    const std::string_view diagnostic_category) const noexcept {
  try {
    if (!interaction.responder) {
      throw std::runtime_error{"Interaction responder is missing."};
    }
    const auto correlation_id = interaction.correlation_id;
    const auto category = std::string{diagnostic_category};
    const auto callbacks = callbacks_;
    interaction.responder->edit_original(
        message, [this, callbacks, correlation_id,
                  category](const DeliveryResult result) {
          try {
            static_cast<void>(callbacks->invoke([this, correlation_id, category,
                                                 result] {
              if (result != DeliveryResult::success) {
                diagnostics_.emit({DiagnosticSeverity::warning, category,
                                   "Discord did not confirm the interaction "
                                   "response.",
                                   correlation_id});
              }
            }));
          } catch (...) {
          }
        });
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error,
                       std::string{diagnostic_category}, error.what(),
                       interaction.correlation_id});
  } catch (...) {
    diagnostics_.emit(
        {DiagnosticSeverity::error, std::string{diagnostic_category},
         "Unknown interaction response failure.", interaction.correlation_id});
  }
}

void InteractionHandler::edit_reveal(
    const IncomingInteraction &interaction, OpenPendingNoticeResult reveal,
    const std::string_view diagnostic_category) const noexcept {
  const auto delivery_key = reveal.delivery_idempotency_key;
  if (!delivery_key.has_value()) {
    edit(interaction, render_private_notice(reveal), diagnostic_category);
    return;
  }

  try {
    if (!interaction.responder) {
      throw std::runtime_error{"Interaction responder is missing."};
    }
    const auto correlation_id = interaction.correlation_id;
    const auto category = std::string{diagnostic_category};
    const auto callbacks = callbacks_;
    interaction.responder->edit_original(
        render_private_notice(reveal),
        [this, callbacks, correlation_id, category,
         delivery_key = *delivery_key](const DeliveryResult result) {
          try {
            static_cast<void>(callbacks->invoke([this, correlation_id, category,
                                                 delivery_key, result] {
              try {
                static_cast<void>(
                    notices_.complete_delivery(delivery_key, result));
              } catch (const std::exception &error) {
                diagnostics_.emit({DiagnosticSeverity::error,
                                   "interaction.delivery_state", error.what(),
                                   correlation_id});
              } catch (...) {
                diagnostics_.emit(
                    {DiagnosticSeverity::error, "interaction.delivery_state",
                     "Unknown notice delivery-state failure.", correlation_id});
              }
              if (result != DeliveryResult::success) {
                diagnostics_.emit(
                    {DiagnosticSeverity::warning, category,
                     "Discord did not confirm the interaction response; "
                     "the notice remains retrievable.",
                     correlation_id});
              }
            }));
          } catch (...) {
          }
        });
  } catch (const std::exception &error) {
    try {
      static_cast<void>(notices_.complete_delivery(
          *delivery_key, DeliveryResult::permanent_failure));
    } catch (...) {
    }
    diagnostics_.emit({DiagnosticSeverity::error,
                       std::string{diagnostic_category}, error.what(),
                       interaction.correlation_id});
  } catch (...) {
    try {
      static_cast<void>(notices_.complete_delivery(
          *delivery_key, DeliveryResult::unknown_outcome));
    } catch (...) {
    }
    diagnostics_.emit(
        {DiagnosticSeverity::error, std::string{diagnostic_category},
         "Unknown interaction response failure.", interaction.correlation_id});
  }
}

} // namespace sanguinius
