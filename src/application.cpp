#include "sanguinius/application.hpp"

#include "sanguinius/ai_generation.hpp"
#include "sanguinius/ai_responder.hpp"
#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/cross_feature_orchestrator.hpp"
#include "sanguinius/durable_work_controls.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/interaction_handler.hpp"
#include "sanguinius/interaction_router.hpp"
#include "sanguinius/message_handler.hpp"
#include "sanguinius/outbox.hpp"
#include "sanguinius/owner_admin.hpp"
#include "sanguinius/reliability_test.hpp"
#include "sanguinius/sanguinius_overview.hpp"
#include "sanguinius/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::int64_t disabled_feature_job_delay_ms = 60'000;
constexpr std::int64_t application_heartbeat_interval_ms = 30'000;

class UnavailableVoiceInputAdapter final : public VoiceInputAdapter {
public:
  [[nodiscard]] VoiceInputCapability capability() const noexcept override {
    return VoiceInputCapability::disabled;
  }

  void start(AudioCallback, EventCallback) override {}

  [[nodiscard]] VoiceInputPresence
  preflight(const VoiceInputArmRequest &) const override {
    return {};
  }

  [[nodiscard]] bool enable_transport(const VoiceInputArmRequest &,
                                      std::stop_token) override {
    return false;
  }

  [[nodiscard]] bool arm(const VoiceInputArmRequest &) override {
    return false;
  }

  void disarm() noexcept override {}

  [[nodiscard]] bool disable_transport() noexcept override { return true; }

  void shutdown() noexcept override {}
};

enum class ApplicationState {
  created,
  starting,
  running,
  stopping,
  stopped,
};

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] bool retired_prefix_command(const std::string_view content,
                                          const std::string_view prefix) {
  if (!content.starts_with(prefix))
    return false;
  const auto start = prefix.size();
  const auto end = content.find_first_of(" \t\r\n", start);
  auto name = content.substr(start, end == std::string_view::npos
                                        ? std::string_view::npos
                                        : end - start);
  std::string normalized{name};
  std::ranges::transform(normalized, normalized.begin(),
                         [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                         });
  return normalized == "help" || normalized == "repo";
}

void validate(const ApplicationDependencies &dependencies) {
  if (!dependencies.clock || !dependencies.id_generator ||
      !dependencies.persistent_id_generator || !dependencies.diagnostics ||
      !dependencies.message_log || !dependencies.application_instances ||
      !dependencies.identities || !dependencies.pending_notices ||
      !dependencies.durable_work || !dependencies.ai_client ||
      !dependencies.discord || !dependencies.reliability_tests) {
    throw std::invalid_argument{
        "Application dependencies must all be configured."};
  }
}

} // namespace

class Application::Impl {
public:
  Impl(ApplicationOptions options, ApplicationDependencies dependencies)
      : options_{std::move(options)}, clock_{std::move(dependencies.clock)},
        id_generator_{std::move(dependencies.id_generator)},
        persistent_id_generator_{
            std::move(dependencies.persistent_id_generator)},
        diagnostics_{std::move(dependencies.diagnostics)},
        message_log_{std::move(dependencies.message_log)},
        application_instances_{std::move(dependencies.application_instances)},
        identities_{std::move(dependencies.identities)},
        pending_notices_{std::move(dependencies.pending_notices)},
        durable_work_{std::move(dependencies.durable_work)},
        chronicle_repository_{std::move(dependencies.chronicle)},
        chronicle_session_repository_{
            std::move(dependencies.chronicle_sessions)},
        relationship_repository_{std::move(dependencies.relationships)},
        appearance_repository_{std::move(dependencies.appearances)},
        tarot_repository_{std::move(dependencies.tarot)},
        wager_repository_{std::move(dependencies.wagers)},
        tarot_catalog_repository_{std::move(dependencies.tarot_catalogs)},
        tarot_draw_repository_{std::move(dependencies.tarot_draws)},
        tarot_house_repository_{std::move(dependencies.tarot_house)},
        tarot_integration_repository_{
            std::move(dependencies.tarot_integration)},
        vox_repository_{std::move(dependencies.vox)},
        speech_repository_{std::move(dependencies.speech)},
        vox_narration_repository_{std::move(dependencies.vox_narration)},
        voice_listening_repository_{std::move(dependencies.voice_listening)},
        random_{std::move(dependencies.random)},
        appearance_policy_{std::move(dependencies.appearance_policy)},
        ai_client_{std::move(dependencies.ai_client)},
        discord_{std::move(dependencies.discord)},
        voice_gateway_{std::move(dependencies.voice_gateway)},
        voice_input_adapter_{std::move(dependencies.voice_input_adapter)},
        transcription_{std::move(dependencies.transcription)},
        text_to_speech_{std::move(dependencies.text_to_speech)},
        audio_normalizer_{std::move(dependencies.audio_normalizer)},
        tts_cache_{std::move(dependencies.tts_cache)},
        runtime_feature_controls_{
            std::move(dependencies.runtime_feature_controls)},
        retention_repository_{std::move(dependencies.retention)},
        reliability_tests_{std::move(dependencies.reliability_tests)} {
    service_notifier_ = std::move(dependencies.service_notifier);
    if (!service_notifier_)
      service_notifier_ = std::make_unique<NoopServiceNotifier>();
    if (!clock_ || !id_generator_ || !persistent_id_generator_ ||
        !diagnostics_ || !message_log_ || !application_instances_ ||
        !identities_ || !pending_notices_ || !durable_work_ || !ai_client_ ||
        !discord_ || !reliability_tests_) {
      throw std::invalid_argument{
          "Application dependencies must all be configured."};
    }
    if (options_.features.vox_enabled &&
        (!vox_repository_ || !speech_repository_ || !voice_gateway_ ||
         !audio_normalizer_ || !tts_cache_)) {
      throw std::invalid_argument{
          "Vox persistence and the voice gateway are required when enabled."};
    }
    if (options_.features.vox_narration_enabled &&
        (!options_.features.vox_enabled || !vox_narration_repository_)) {
      throw std::invalid_argument{
          "Vox narration requires Vox output and narration persistence."};
    }
    if (options_.features.voice_input_enabled &&
        (!options_.features.vox_enabled || !voice_listening_repository_ ||
         !voice_input_adapter_)) {
      throw std::invalid_argument{
          "Voice-input persistence and adapter are required when enabled."};
    }

    scope_policy_ = std::make_unique<ServerScopePolicy>(options_.server_scope);
    notice_service_ = std::make_unique<PendingNoticeService>(
        *pending_notices_, *clock_, *persistent_id_generator_);
    outbox_ = std::make_unique<OutboxService>(
        *durable_work_, *clock_, *persistent_id_generator_, *diagnostics_,
        *discord_, *discord_, options_.server_scope, options_.instance_id, 32,
        options_.durable_delivery_receipt_wait, [this] {
          if (cross_feature_orchestrator_)
            cross_feature_orchestrator_->wake();
        });
    if (options_.features.chronicle_enabled &&
        (!chronicle_repository_ || !chronicle_session_repository_)) {
      throw std::invalid_argument{
          "Chronicle persistence is required when the feature is enabled."};
    }
    ai_work_ = std::make_unique<AiWorkService>(options_.ai_queue_capacity,
                                               options_.ai_worker_count);
    if (options_.features.appearances_mode != AppearanceMode::off &&
        (!appearance_repository_ || !appearance_policy_)) {
      throw std::invalid_argument{
          "Appearance persistence is required when appearances are enabled."};
    }
    if (options_.features.tarot_enabled && (!tarot_repository_ || !random_)) {
      throw std::invalid_argument{
          "Tarot persistence and randomness are required when enabled."};
    }
    if (options_.features.tarot_enabled && !wager_repository_) {
      throw std::invalid_argument{
          "Wager persistence is required when the Tarot is enabled."};
    }
    const bool tarot_experience_configured =
        tarot_catalog_repository_ && tarot_draw_repository_ &&
        tarot_house_repository_ && options_.tarot_deck_catalog &&
        options_.tarot_house_catalog;
    if (options_.features.tarot_enabled &&
        options_.tarot_house_policy.house_enabled &&
        !tarot_experience_configured) {
      throw std::invalid_argument{
          "Tarot catalog, draw, and House persistence are required when "
          "House play is enabled."};
    }
    if (options_.features.tarot_enabled) {
      tarot_service_ = std::make_unique<TarotService>(
          *tarot_repository_, *clock_, *persistent_id_generator_, *random_,
          options_.tarot_policy, options_.server_scope,
          options_.controls.test_mode, *diagnostics_,
          [this] { outbox_->wake(); },
          [this](const std::string_view) {
            if (cross_feature_orchestrator_)
              cross_feature_orchestrator_->wake();
          });
      wager_service_ = std::make_unique<TarotWagerService>(
          *wager_repository_, *clock_, *persistent_id_generator_,
          options_.wager_policy, options_.tarot_policy.starting_fate,
          options_.server_scope, options_.controls.test_mode, *diagnostics_,
          [this] {
            if (scheduler_)
              scheduler_->wake();
          },
          [this] { outbox_->wake(); },
          [this](const std::string_view) {
            if (cross_feature_orchestrator_)
              cross_feature_orchestrator_->wake();
          });
      if (tarot_experience_configured) {
        if (options_.tarot_house_policy.integration_enabled &&
            !tarot_integration_repository_)
          throw std::invalid_argument{
              "Tarot integration persistence is required when enabled."};
        if (tarot_integration_repository_) {
          tarot_integration_service_ =
              std::make_unique<TarotIntegrationService>(
                  *tarot_integration_repository_, *clock_,
                  *persistent_id_generator_, *diagnostics_,
                  options_.tarot_house_policy.integration_enabled,
                  TarotIntegrationSinkPolicy{
                      .chronicle_enabled =
                          options_.features.chronicle_enabled});
        }
        tarot_house_service_ = std::make_unique<TarotHouseService>(
            *tarot_house_repository_, *tarot_repository_,
            *options_.tarot_house_catalog, *clock_, *persistent_id_generator_,
            options_.tarot_house_policy, options_.tarot_policy,
            options_.server_scope, options_.controls.test_mode, *diagnostics_,
            [this](const std::string_view) {
              outbox_->wake();
              if (cross_feature_orchestrator_)
                cross_feature_orchestrator_->wake();
            });
        tarot_draw_service_ = std::make_unique<TarotDrawService>(
            *tarot_draw_repository_, *options_.tarot_deck_catalog, *clock_,
            *persistent_id_generator_, *random_, options_.tarot_house_policy,
            options_.server_scope, options_.controls.test_mode, *diagnostics_,
            [this](const TarotDrawRecord &) {
              outbox_->wake();
              if (cross_feature_orchestrator_)
                cross_feature_orchestrator_->wake();
            });
      }
    }
    if (appearance_repository_ && appearance_policy_) {
      appearance_service_ = std::make_unique<AppearanceService>(
          *appearance_repository_, *clock_, *persistent_id_generator_,
          std::move(*appearance_policy_), options_.features.appearances_mode,
          options_.instance_id, ai_client_.get(), ai_work_.get(), *diagnostics_,
          options_.persona, options_.timezone,
          [this] {
            const auto discord_status = discord_->status();
            const auto queue = ai_work_->snapshot();
            return AppearanceRuntimeState{
                .operational = discord_status.ready && queue.accepting,
                .degraded = !discord_status.ready || !queue.accepting ||
                            queue.queued >= queue.capacity ||
                            discord_status.command_registration ==
                                CommandRegistrationState::failed};
          },
          [this] { outbox_->wake(); });
    }
    if (options_.features.chronicle_enabled && !relationship_repository_) {
      throw std::invalid_argument{"Relationship persistence is required when "
                                  "the Chronicle is enabled."};
    }
    if (relationship_repository_ && options_.features.chronicle_enabled) {
      relationship_service_ = std::make_unique<RelationshipService>(
          *relationship_repository_, *clock_, *persistent_id_generator_,
          options_.server_scope, options_.instance_id);
    }
    ai_responder_ = std::make_unique<AiResponder>(
        *ai_client_, *ai_work_, *discord_, *discord_, *diagnostics_,
        options_.persona,
        options_.features.chronicle_enabled ? relationship_service_.get()
                                            : nullptr,
        options_.features);
    if (chronicle_repository_) {
      chronicle_service_ = std::make_unique<ChronicleService>(
          *chronicle_repository_, *clock_, *persistent_id_generator_,
          options_.server_scope, options_.controls, [this] { outbox_->wake(); },
          [this] {
            if (scheduler_)
              scheduler_->wake();
          },
          64,
          [this] {
            if (cross_feature_orchestrator_)
              cross_feature_orchestrator_->wake();
          },
          [this](const ContextMessageSnapshot &message)
              -> std::optional<std::pair<std::string, bool>> {
            if (!appearance_service_)
              return std::nullopt;
            const auto verified =
                appearance_service_->verify_public_delivery(message);
            if (!verified)
              return std::nullopt;
            return std::pair{verified->decision_id, verified->test_delivery};
          });
    }
    if (options_.features.chronicle_enabled && chronicle_session_repository_) {
      chronicle_session_service_ = std::make_unique<ChronicleSessionService>(
          *chronicle_session_repository_, *clock_, *persistent_id_generator_,
          options_.server_scope, options_.controls,
          [this] {
            if (scheduler_)
              scheduler_->wake();
          },
          [this] { outbox_->wake(); }, options_.timezone, ai_client_.get(),
          ai_work_.get(), durable_work_.get(), diagnostics_.get(),
          [this] {
            if (cross_feature_orchestrator_)
              cross_feature_orchestrator_->wake();
          });
    }
    scheduler_ = std::make_unique<SchedulerService>(
        *durable_work_, *clock_, *persistent_id_generator_, *diagnostics_,
        options_.instance_id, [this] { outbox_->wake(); }, 32,
        chronicle_service_
            ? JobHandlerRegistry::Handler{[this](
                                              const ClaimedScheduledJob &job) {
                const auto result = chronicle_service_->complete_expiry(job);
                if (result.code == ChronicleResultCode::invalid_state) {
                  throw std::invalid_argument{
                      "Invalid Chronicle memory-expiry payload."};
                }
              }}
            : JobHandlerRegistry::Handler{});
    if (options_.features.vox_enabled) {
      speech_service_ = std::make_unique<SpeechService>(
          *speech_repository_, text_to_speech_.get(), *audio_normalizer_,
          *tts_cache_, *voice_gateway_, *clock_, *persistent_id_generator_,
          *diagnostics_, std::move(options_.static_speech_assets),
          options_.speech,
          [this](std::string provider_nonce, std::string message) {
            discord_->send_public(
                {.guild_id = options_.server_scope.guild_id,
                 .channel_id = options_.server_scope.primary_channel_id,
                 .message = text_message(std::move(message))},
                discord_nonce_from_uuid(provider_nonce),
                [this](const PublicDeliveryReceipt receipt) {
                  if (receipt.result != DeliveryResult::success) {
                    diagnostics_->emit(
                        {DiagnosticSeverity::warning,
                         "vox.tts.text_fallback",
                         "A public-safe Vox failure notice was not delivered.",
                         {}});
                  }
                });
          },
          [this] {
            return !options_.features.vox_narration_enabled ||
                   (vox_narration_repository_ &&
                    vox_narration_repository_->automatic_speech_suppressed(
                        unix_milliseconds(*clock_)));
          });
      vox_service_ = std::make_unique<VoxService>(
          *vox_repository_, *voice_gateway_, *clock_, *persistent_id_generator_,
          *diagnostics_, options_.server_scope, options_.controls,
          options_.instance_id,
          [this] {
            if (scheduler_)
              scheduler_->wake();
          },
          [this] { outbox_->wake(); }, vox_worker_capacity,
          speech_service_.get(), options_.features.vox_narration_enabled,
          [this] {
            return vox_narration_repository_ &&
                   vox_narration_repository_->automatic_speech_suppressed(
                       unix_milliseconds(*clock_));
          },
          [this](std::string session_id, std::string guild_id,
                 std::string summoner_user_id) {
            if (vox_narration_service_)
              vox_narration_service_->prepare_session_flavor(
                  std::move(session_id), std::move(guild_id),
                  std::move(summoner_user_id));
          });
    }
    if (vox_narration_repository_)
      vox_narration_service_ = std::make_unique<VoxNarrationService>(
          *vox_narration_repository_, *clock_, *persistent_id_generator_,
          *diagnostics_, *ai_client_, *ai_work_, options_.instance_id,
          options_.features.vox_narration_enabled, options_.controls.test_mode,
          [this] {
            if (speech_service_)
              speech_service_->wake();
          },
          [this](std::string session_id, std::string guild_id,
                 std::string entrance, std::string farewell) {
            if (speech_service_)
              speech_service_->prepare_session_flavor(
                  std::move(session_id), std::move(guild_id),
                  std::move(entrance), std::move(farewell));
          },
          [this] {
            if (cross_feature_orchestrator_)
              cross_feature_orchestrator_->wake();
          });
    if (voice_listening_repository_ && !voice_input_adapter_)
      voice_input_adapter_ = std::make_unique<UnavailableVoiceInputAdapter>();
    if (voice_listening_repository_) {
      voice_listening_service_ = std::make_unique<VoiceListeningService>(
          *voice_listening_repository_, *voice_input_adapter_,
          transcription_.get(), *discord_, *clock_, *persistent_id_generator_,
          *diagnostics_, options_.server_scope, options_.voice_input,
          [this]() -> std::optional<ActiveVoxListeningContext> {
            if (!vox_service_)
              return std::nullopt;
            const auto current = vox_service_->active_session();
            if (!current)
              return std::nullopt;
            const auto health = vox_service_->health();
            bool speech_idle = true;
            if (health.speech) {
              const auto &speech = *health.speech;
              speech_idle = speech.repository.queued == 0 &&
                            speech.repository.synthesizing == 0 &&
                            speech.repository.ready == 0 &&
                            speech.repository.playing == 0 &&
                            speech.synthesis_worker.queued == 0 &&
                            speech.playback_worker.queued == 0;
            }
            return ActiveVoxListeningContext{
                .session_id = current->session_id,
                .guild_id = current->guild_id,
                .text_channel_id = current->text_channel_id,
                .voice_channel_id = current->voice_channel_id,
                .connection_generation = current->connection_generation,
                .ready = current->state == VoxState::ready,
                .speech_idle = speech_idle};
          },
          chronicle_service_.get(),
          [this](const bool listening) {
            if (speech_service_)
              speech_service_->set_voice_input_listening(listening);
          });
    }
    if (runtime_feature_controls_) {
      safety_controls_ = std::make_unique<SafetyControlService>(
          std::move(runtime_feature_controls_), *clock_,
          *persistent_id_generator_, options_.features,
          dynamic_cast<AiGenerationService *>(ai_client_.get()),
          appearance_service_.get(), speech_service_.get(), vox_service_.get(),
          voice_listening_service_.get());
    }
    if (retention_repository_) {
      retention_service_ = std::make_unique<RetentionService>(
          std::move(retention_repository_), *clock_, *persistent_id_generator_,
          speech_repository_.get(), tts_cache_.get());
      scheduler_->add_handler(
          std::string{retention_job_type},
          [this](const ClaimedScheduledJob &job) {
            if (!std::holds_alternative<std::monostate>(job.payload))
              throw std::invalid_argument{"Invalid retention payload."};
            static_cast<void>(retention_service_->run());
            const auto current = unix_milliseconds(*clock_);
            if (durable_work_->reschedule_job(
                    job, current, RetentionService::next_due_utc(current)) !=
                WorkMutationStatus::applied)
              throw std::runtime_error{"Retention claim became stale."};
          });
    }
    const auto add_vox_handler = [this](const std::string_view job_type) {
      scheduler_->add_handler(
          std::string{job_type}, [this](const ClaimedScheduledJob &job) {
            if (!vox_service_) {
              const auto current = unix_milliseconds(*clock_);
              static_cast<void>(durable_work_->defer_job(
                  job, current, current + disabled_feature_job_delay_ms,
                  "feature_disabled"));
              return;
            }
            if (vox_service_->handle_timeout(job) != SubmitResult::accepted) {
              throw std::runtime_error{"The Vox timeout queue is unavailable."};
            }
          });
    };
    add_vox_handler(vox_connect_timeout_job_type);
    add_vox_handler(vox_reconnect_timeout_job_type);
    add_vox_handler(vox_leave_timeout_job_type);
    add_vox_handler(vox_empty_timeout_job_type);
    add_vox_handler(vox_mute_expiry_job_type);
    scheduler_->add_handler(std::string{wager_deadline_job_type},
                            [this](const ClaimedScheduledJob &job) {
                              if (!wager_service_) {
                                const auto current = unix_milliseconds(*clock_);
                                static_cast<void>(durable_work_->defer_job(
                                    job, current,
                                    current + disabled_feature_job_delay_ms,
                                    "feature_disabled"));
                                return;
                              }
                              wager_service_->handle_deadline(job);
                            });
    scheduler_->add_handler(std::string{tarot_house_deadline_job_type},
                            [this](const ClaimedScheduledJob &job) {
                              if (!tarot_house_service_) {
                                const auto current = unix_milliseconds(*clock_);
                                static_cast<void>(durable_work_->defer_job(
                                    job, current,
                                    current + disabled_feature_job_delay_ms,
                                    "feature_disabled"));
                                return;
                              }
                              tarot_house_service_->handle_deadline(job);
                            });
    scheduler_->add_handler(std::string{tarot_house_offer_expiry_job_type},
                            [this](const ClaimedScheduledJob &job) {
                              if (!tarot_house_service_ ||
                                  !options_.tarot_house_policy.house_enabled) {
                                const auto current = unix_milliseconds(*clock_);
                                static_cast<void>(durable_work_->defer_job(
                                    job, current,
                                    current + disabled_feature_job_delay_ms,
                                    "feature_disabled"));
                                return;
                              }
                              tarot_house_service_->handle_offer_expiry(job);
                            });
    scheduler_->add_handler(
        std::string{tarot_integration_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (cross_feature_orchestrator_)
            cross_feature_orchestrator_->wake();
          const auto current = unix_milliseconds(*clock_);
          if (durable_work_->cancel_claimed_job(job, current) !=
              WorkMutationStatus::applied)
            throw std::runtime_error{
                "Obsolete Tarot integration scan claim became stale."};
        });
    scheduler_->add_handler(
        std::string{tarot_house_weekly_offer_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!tarot_house_service_ ||
              !options_.tarot_house_policy.house_enabled) {
            const auto current = unix_milliseconds(*clock_);
            if (durable_work_->reschedule_job(
                    job, current,
                    next_house_weekly_offer_ms(current,
                                               tarot_house_timezone)) !=
                WorkMutationStatus::applied)
              throw std::runtime_error{
                  "Disabled House weekly offer claim became stale."};
            return;
          }
          const auto status = discord_->status();
          const auto result = tarot_house_service_->handle_weekly_offer(
              job, status.ready && status.command_registration !=
                                       CommandRegistrationState::failed);
          if (result.status == HouseWeeklyOfferStatus::deferred) {
            const auto current = unix_milliseconds(*clock_);
            const auto mutation =
                current < job.due_at_ms
                    ? durable_work_->defer_job(job, current, job.due_at_ms,
                                               "wall_clock_rollback")
                    : durable_work_->release_job(job, current);
            if (mutation != WorkMutationStatus::applied)
              throw std::runtime_error{
                  "Deferred House weekly offer claim became stale."};
            return;
          }
          if (result.outbox_created)
            outbox_->wake();
          const auto current = unix_milliseconds(*clock_);
          if (durable_work_->reschedule_job(
                  job, current,
                  next_house_weekly_offer_ms(current, tarot_house_timezone)) !=
              WorkMutationStatus::applied)
            throw std::runtime_error{"House weekly offer claim became stale."};
        });
    scheduler_->add_handler(
        std::string{session_summary_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!chronicle_session_service_) {
            const auto current = unix_milliseconds(*clock_);
            static_cast<void>(durable_work_->defer_job(
                job, current, current + disabled_feature_job_delay_ms,
                "feature_disabled"));
            return;
          }
          if (chronicle_session_service_->submit_summary_job(job) !=
              SubmitResult::accepted)
            throw std::runtime_error{"The shared AI queue is unavailable."};
        });
    scheduler_->add_handler(
        std::string{session_context_purge_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!chronicle_session_repository_) {
            const auto current = unix_milliseconds(*clock_);
            static_cast<void>(durable_work_->defer_job(
                job, current, current + disabled_feature_job_delay_ms,
                "repository_unavailable"));
            return;
          }
          const auto result = chronicle_session_repository_->purge_context_job(
              job, unix_milliseconds(*clock_));
          if (result == WorkMutationStatus::invalid_state)
            throw std::invalid_argument{"Invalid context-purge payload."};
        });
    scheduler_->add_handler(
        std::string{anniversary_scan_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!chronicle_session_service_) {
            const auto current = unix_milliseconds(*clock_);
            static_cast<void>(durable_work_->defer_job(
                job, current, current + disabled_feature_job_delay_ms,
                "feature_disabled"));
            return;
          }
          const auto result =
              chronicle_session_service_->handle_anniversary_job(job);
          if (result.status == WorkMutationStatus::invalid_state)
            throw std::invalid_argument{"Invalid anniversary payload."};
        });
    scheduler_->add_handler(
        std::string{appearance_scan_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!std::holds_alternative<AppearanceScanJobPayload>(job.payload))
            throw std::invalid_argument{"Invalid appearance scan payload."};
          if (cross_feature_orchestrator_)
            cross_feature_orchestrator_->wake();
          const auto current = unix_milliseconds(*clock_);
          const auto status = durable_work_->cancel_claimed_job(job, current);
          if (status != WorkMutationStatus::applied)
            throw std::runtime_error{
                "Obsolete appearance scan claim became stale."};
        });
    scheduler_->add_handler(
        std::string{appearance_purge_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!std::holds_alternative<AppearancePurgeJobPayload>(job.payload))
            throw std::invalid_argument{"Invalid appearance purge payload."};
          const auto recurrence_ms =
              appearance_service_ ? appearance_service_->purge_interval_ms()
                                  : appearance_maximum_purge_interval_ms;
          if (appearance_service_)
            appearance_service_->purge();
          const auto current = unix_milliseconds(*clock_);
          const auto status = durable_work_->reschedule_job(
              job, current, current + recurrence_ms);
          if (status != WorkMutationStatus::applied)
            throw std::runtime_error{"Appearance purge claim became stale."};
        });
    scheduler_->add_handler(
        std::string{vox_tts_purge_job_type},
        [this](const ClaimedScheduledJob &job) {
          if (!std::holds_alternative<std::monostate>(job.payload))
            throw std::invalid_argument{"Invalid TTS purge payload."};
          if (speech_service_)
            static_cast<void>(speech_service_->purge());
          const auto current = unix_milliseconds(*clock_);
          const auto status = durable_work_->reschedule_job(
              job, current, current + vox_tts_purge_interval_ms);
          if (status != WorkMutationStatus::applied)
            throw std::runtime_error{"TTS purge claim became stale."};
        });
    cross_feature_orchestrator_ =
        std::make_unique<CrossFeatureOrchestrator>(*diagnostics_);
    if (relationship_service_) {
      cross_feature_orchestrator_->add_consumer("relationships", [this] {
        return relationship_service_->recover(50) >= 50;
      });
    }
    if (tarot_house_service_) {
      cross_feature_orchestrator_->add_consumer("tarot_house", [this] {
        return tarot_house_service_->reconcile_draws(50);
      });
    }
    if (tarot_integration_service_) {
      cross_feature_orchestrator_->add_consumer("tarot_integration", [this] {
        if (!tarot_integration_service_->enabled())
          return tarot_integration_service_->suppress_disabled_batch(50);
        const auto report = tarot_integration_service_->scan();
        return report.pending != 0;
      });
    }
    if (appearance_service_) {
      cross_feature_orchestrator_->add_consumer("appearances", [this] {
        return appearance_service_->scan_event_batch(50);
      });
    }
    if (vox_narration_service_) {
      cross_feature_orchestrator_->add_consumer("vox_narration", [this] {
        return vox_narration_service_->run_one_cycle();
      });
    }
    cross_feature_orchestrator_->add_consumer("application_heartbeat", [this] {
      const auto current = unix_milliseconds(*clock_);
      if (current < last_application_heartbeat_ms_ ||
          current - last_application_heartbeat_ms_ >=
              application_heartbeat_interval_ms) {
        application_instances_->record_heartbeat(options_.instance_id, current);
        last_application_heartbeat_ms_ = current;
      }
      return false;
    });

    message_observation_pipeline_ =
        std::make_unique<MessageObservationPipeline>(*diagnostics_);
    message_observation_pipeline_->add_observer(
        "identity", [this](const IncomingMessage &message) {
          const bool primary_scope =
              message.guild_id == options_.server_scope.guild_id &&
              message.channel_id == options_.server_scope.primary_channel_id;
          if (!primary_scope)
            return;
          identities_->ensure_user(DiscordUserRecord{
              .user_id = message.author_user_id,
              .display_name =
                  message.author_display_name.empty()
                      ? std::nullopt
                      : std::optional<std::string>{message.author_display_name},
              .username =
                  message.author_username.empty()
                      ? std::nullopt
                      : std::optional<std::string>{message.author_username},
              .is_bot = message.author_is_bot,
              .observed_at_ms = unix_milliseconds(*clock_),
          });
        });
    if (chronicle_session_service_) {
      message_observation_pipeline_->add_observer(
          "chronicle_session", [this](const IncomingMessage &message) {
            chronicle_session_service_->observe_message(message);
          });
    }
    if (appearance_service_) {
      message_observation_pipeline_->add_observer(
          "appearances", [this](const IncomingMessage &message) {
            const bool primary_scope =
                message.guild_id == options_.server_scope.guild_id &&
                message.channel_id == options_.server_scope.primary_channel_id;
            const bool observable =
                !message.author_is_bot ||
                (message.bot_user_id.is_set() &&
                 message.author_user_id == message.bot_user_id);
            const bool deprecated_command = retired_prefix_command(
                message.content, options_.command_prefix);
            const bool direct_invocation =
                !message.author_is_bot &&
                (deprecated_command ||
                 parse_admin_operation(message.content, options_.command_prefix)
                     .has_value() ||
                 ai_responder_->handles(message));
            if (!primary_scope || !observable || direct_invocation)
              return;
            appearance_service_->observe_message(AppearanceMessageObservation{
                .message_id = message.message_id,
                .guild_id = message.guild_id,
                .channel_id = message.channel_id,
                .author_user_id = message.author_user_id,
                .author_is_bot = message.author_is_bot,
                .excerpt = message.content,
                .observed_at_ms = unix_milliseconds(*clock_),
                .correlation_id = message.correlation_id,
            });
          });
    }
    durable_controls_ = std::make_unique<DurableWorkControlService>(
        *durable_work_, *clock_, *persistent_id_generator_,
        options_.server_scope, [this] { scheduler_->wake(); },
        [this] { outbox_->wake(); });
    health_service_ = std::make_unique<HealthService>(
        options_.build, options_.controls, options_.features,
        options_.persistence,
        HealthRuntimeProviders{
            .discord_status = discord_.get(),
            .interaction_queue =
                [this] {
                  return interaction_handler_ == nullptr
                             ? QueueSnapshot{}
                             : interaction_handler_->queue_snapshot();
                },
            .scheduler_queue = [this] { return scheduler_->queue_snapshot(); },
            .outbox_queue = [this] { return outbox_->queue_snapshot(); },
            .pending_notice_count =
                [this] { return notice_service_->pending_count_all(); },
            .durable_work =
                [this] {
                  return durable_work_->health(unix_milliseconds(*clock_));
                },
            .tarot = [this]() -> std::optional<TarotInvariantReport> {
              return tarot_service_
                         ? std::optional<
                               TarotInvariantReport>{tarot_service_
                                                         ->check_invariants()}
                         : std::nullopt;
            },
            .wagers = [this]() -> std::optional<WagerInvariantReport> {
              return wager_service_
                         ? std::optional<
                               WagerInvariantReport>{wager_service_
                                                         ->check_invariants()}
                         : std::nullopt;
            },
            .house = [this]() -> std::optional<HouseEconomyReport> {
              return tarot_house_service_
                         ? std::optional<
                               HouseEconomyReport>{tarot_house_service_
                                                       ->check_invariants()}
                         : std::nullopt;
            },
            .vox = [this]() -> std::optional<VoxHealth> {
              return vox_service_ ? std::optional{vox_service_->health()}
                                  : std::nullopt;
            },
            .vox_narration = [this]() -> std::optional<VoxNarrationHealth> {
              return vox_narration_service_
                         ? std::optional{vox_narration_service_->health()}
                         : std::nullopt;
            },
            .voice_input = [this]() -> std::optional<VoiceListeningHealth> {
              return voice_listening_service_
                         ? std::optional{voice_listening_service_->health()}
                         : std::nullopt;
            },
            .cross_feature = [this]() -> std::optional<CrossFeatureHealth> {
              return cross_feature_orchestrator_
                         ? std::optional{cross_feature_orchestrator_->health()}
                         : std::nullopt;
            },
            .operations =
                [this] {
                  return read_operations_health(
                      options_.operations_status_file, options_.state_directory,
                      options_.cache_directory, options_.backup_directory,
                      unix_milliseconds(*clock_));
                },
        });
    owner_admin_ = std::make_unique<OwnerAdminService>(
        options_.controls, *scope_policy_, *health_service_);
    overview_service_ =
        std::make_unique<SanguiniusOverviewService>(options_.features, *clock_);
    message_handler_ = std::make_unique<MessageHandler>(
        *message_log_, *ai_responder_, *discord_, *diagnostics_, *owner_admin_,
        options_.command_prefix, options_.message_queue_capacity,
        [this](const IncomingMessage &message) {
          message_observation_pipeline_->observe(message);
        },
        [this](const HealthSnapshot &snapshot) {
          return bounded_health_message(overview_service_->owner_health(
              render_health(snapshot),
              appearance_service_ ? appearance_service_->member_status_summary()
                                  : "unavailable",
              safety_controls_ ? safety_controls_->member_status()
                               : MemberRuntimeStatus{},
              appearance_service_ ? appearance_service_->status_summary()
                                  : std::string_view{}));
        });
    interaction_handler_ = std::make_unique<InteractionHandler>(
        *identities_, *notice_service_, *clock_, *durable_controls_,
        chronicle_service_.get(), chronicle_session_service_.get(),
        *health_service_, *diagnostics_, options_.features, *overview_service_,
        [this] { return message_handler_->queue_snapshot(); },
        [this] { return ai_responder_->queue_snapshot(); },
        options_.interaction_queue_capacity, relationship_service_.get(),
        appearance_service_.get(), tarot_service_.get(), wager_service_.get(),
        tarot_draw_service_.get(), tarot_house_service_.get(),
        tarot_integration_service_.get(), vox_service_.get(),
        vox_narration_service_.get(), voice_listening_service_.get(),
        safety_controls_.get(),
        [this](const std::string_view scenario) {
          return reliability_tests_->run(scenario);
        });
    interaction_router_ = std::make_unique<InteractionRouter>(
        *scope_policy_, options_.controls, options_.features,
        *interaction_handler_, *diagnostics_);
  }

  void start() {
    {
      const std::scoped_lock lock{state_mutex_};
      if (state_ != ApplicationState::created) {
        throw std::logic_error{"Application may only be started once."};
      }
      state_ = ApplicationState::starting;
    }

    try {
      service_notifier_->status("Starting runtime components");
      const auto started_at_ms = unix_milliseconds(*clock_);
      application_instances_->record_start(ApplicationInstanceRecord{
          .instance_id = options_.instance_id,
          .application_version = options_.build.version,
          .git_revision = options_.build.revision,
          .hostname = options_.hostname,
          .process_id = options_.process_id,
          .started_at_ms = started_at_ms,
      });
      last_application_heartbeat_ms_ = started_at_ms;
      instance_started_ = true;
      if (vox_repository_) {
        static_cast<void>(vox_repository_->recover(
            options_.instance_id, unix_milliseconds(*clock_),
            persistent_id_generator_->next_id(),
            persistent_id_generator_->next_id()));
      }
      if (tarot_service_)
        tarot_service_->initialize();
      if (tarot_catalog_repository_ && options_.tarot_deck_catalog &&
          options_.tarot_house_catalog) {
        tarot_catalog_repository_->install(*options_.tarot_deck_catalog,
                                           *options_.tarot_house_catalog,
                                           unix_milliseconds(*clock_));
      }
      if (wager_service_ && !wager_service_->check_invariants().valid) {
        throw std::runtime_error{
            "Wager escrow invariant verification failed during startup."};
      }
      if (tarot_house_service_ &&
          !tarot_house_service_->check_invariants().valid) {
        throw std::runtime_error{
            "House escrow invariant verification failed during startup."};
      }
      if (tarot_house_service_)
        tarot_house_service_->ensure_weekly_schedule();
      if (appearance_service_)
        appearance_service_->start();
      static_cast<void>(notice_service_->recover_incomplete_deliveries());
      if (chronicle_session_service_)
        chronicle_session_service_->ensure_anniversary_schedule();
      outbox_->start();
      outbox_started_ = true;
      ai_work_->start();
      ai_started_ = true;
      ai_responder_->start();
      scheduler_->start();
      scheduler_started_ = true;
      message_handler_->start();
      message_handler_started_ = true;
      interaction_handler_->start();
      interaction_handler_started_ = true;
      if (speech_service_) {
        speech_service_->start();
        speech_started_ = true;
      }
      if (vox_narration_service_) {
        vox_narration_service_->start();
        vox_narration_started_ = true;
      }
      if (vox_service_) {
        vox_service_->start();
        vox_started_ = true;
      }
      if (voice_listening_service_) {
        voice_listening_service_->start();
        voice_listening_started_ = true;
      }
      cross_feature_orchestrator_->start();
      cross_feature_started_ = true;
      discord_->start(
          [this](IncomingMessage message) {
            try {
              message.correlation_id = id_generator_->next_id();
              static_cast<void>(message_handler_->enqueue(std::move(message)));
            } catch (const std::exception &error) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "message.intake",
                                  error.what(),
                                  {}});
            } catch (...) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "message.intake",
                                  "Unknown message intake failure.",
                                  {}});
            }
          },
          [this](IncomingInteraction interaction) {
            try {
              interaction.correlation_id = id_generator_->next_id();
              interaction_router_->route(std::move(interaction));
            } catch (const std::exception &error) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "interaction.intake",
                                  error.what(),
                                  {}});
            } catch (...) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "interaction.intake",
                                  "Unknown interaction intake failure.",
                                  {}});
            }
          },
          command_catalog(options_.controls.admin_commands_enabled,
                          options_.features.chronicle_enabled,
                          options_.features.tarot_enabled,
                          options_.features.vox_enabled,
                          options_.controls.test_mode),
          [this](const DiscordRuntimeStatus status) {
            if (status.ready && status.command_registration ==
                                    CommandRegistrationState::synchronized) {
              if (!service_ready_notified_.exchange(true)) {
                service_notifier_->ready(
                    "Ready; Discord connected and commands synchronized");
              } else {
                service_notifier_->watchdog(
                    "Ready; Discord connected and commands synchronized");
              }
            } else if (status.command_registration ==
                       CommandRegistrationState::failed) {
              service_notifier_->status(
                  "Discord connected; command synchronization failed");
            } else if (status.ready) {
              service_notifier_->status(
                  "Discord connected; synchronizing commands");
            } else {
              service_notifier_->status("Waiting for Discord readiness");
            }
          });
      const std::scoped_lock lock{state_mutex_};
      state_ = ApplicationState::running;
    } catch (...) {
      stop_components();
      finish_instance(ApplicationStopReason::startup_failure);
      {
        const std::scoped_lock lock{state_mutex_};
        state_ = ApplicationState::stopped;
      }
      throw;
    }
  }

  void stop() noexcept {
    {
      const std::scoped_lock lock{state_mutex_};
      if (state_ == ApplicationState::stopping ||
          state_ == ApplicationState::stopped) {
        return;
      }
      state_ = ApplicationState::stopping;
    }

    service_notifier_->stopping();
    stop_components();
    finish_instance(ApplicationStopReason::clean_shutdown);

    const std::scoped_lock lock{state_mutex_};
    state_ = ApplicationState::stopped;
  }

private:
  void stop_components() noexcept {
    discord_->stop_accepting();
    if (cross_feature_started_) {
      cross_feature_orchestrator_->stop();
      cross_feature_started_ = false;
    }
    if (interaction_router_) {
      interaction_router_->stop();
    }
    if (interaction_handler_started_) {
      // Drain queued safety work while VoiceListeningService remains alive,
      // but keep private response delivery available for its terminal privacy
      // overwrite during shutdown.
      interaction_handler_->drain();
    }
    if (scheduler_started_) {
      scheduler_->stop();
      scheduler_started_ = false;
    }
    if (vox_narration_started_) {
      vox_narration_service_->stop();
      vox_narration_started_ = false;
    }
    if (voice_listening_started_) {
      voice_listening_service_->stop();
      voice_listening_started_ = false;
    }
    if (interaction_handler_started_) {
      interaction_handler_->close_delivery();
      interaction_handler_started_ = false;
    }
    if (vox_started_) {
      vox_service_->stop();
      vox_started_ = false;
    }
    if (speech_started_) {
      speech_service_->stop();
      speech_started_ = false;
    }
    if (outbox_started_) {
      outbox_->stop();
      outbox_started_ = false;
    }
    if (message_handler_started_) {
      message_handler_->stop();
      message_handler_started_ = false;
    }
    if (ai_started_) {
      ai_responder_->stop();
      ai_work_->stop();
      ai_started_ = false;
    }
    discord_->shutdown();
  }

  void finish_instance(const ApplicationStopReason reason) noexcept {
    if (!instance_started_ || instance_finished_) {
      return;
    }
    try {
      application_instances_->record_stop(options_.instance_id,
                                          unix_milliseconds(*clock_), reason);
      instance_finished_ = true;
    } catch (const std::exception &error) {
      diagnostics_->emit({DiagnosticSeverity::error,
                          "database.application_instance",
                          error.what(),
                          {}});
    } catch (...) {
      diagnostics_->emit({DiagnosticSeverity::error,
                          "database.application_instance",
                          "Unknown application instance update failure.",
                          {}});
    }
  }

  ApplicationOptions options_;
  std::unique_ptr<Clock> clock_;
  std::unique_ptr<IdGenerator> id_generator_;
  std::unique_ptr<PersistentIdGenerator> persistent_id_generator_;
  std::unique_ptr<Diagnostics> diagnostics_;
  std::unique_ptr<MessageLog> message_log_;
  std::unique_ptr<ApplicationInstanceRepository> application_instances_;
  std::unique_ptr<CoreIdentityRepository> identities_;
  std::unique_ptr<PendingNoticeRepository> pending_notices_;
  std::unique_ptr<DurableWorkRepository> durable_work_;
  std::unique_ptr<ChronicleRepository> chronicle_repository_;
  std::unique_ptr<ChronicleSessionRepository> chronicle_session_repository_;
  std::unique_ptr<RelationshipRepository> relationship_repository_;
  std::unique_ptr<AppearanceRepository> appearance_repository_;
  std::unique_ptr<TarotRepository> tarot_repository_;
  std::unique_ptr<TarotWagerRepository> wager_repository_;
  std::unique_ptr<TarotCatalogRepository> tarot_catalog_repository_;
  std::unique_ptr<TarotDrawRepository> tarot_draw_repository_;
  std::unique_ptr<TarotHouseRepository> tarot_house_repository_;
  std::unique_ptr<TarotIntegrationRepository> tarot_integration_repository_;
  std::unique_ptr<VoxRepository> vox_repository_;
  std::unique_ptr<SpeechRepository> speech_repository_;
  std::unique_ptr<VoxNarrationRepository> vox_narration_repository_;
  std::unique_ptr<VoiceListeningRepository> voice_listening_repository_;
  std::unique_ptr<Random> random_;
  std::optional<AppearancePolicy> appearance_policy_;
  std::unique_ptr<AiClient> ai_client_;
  std::unique_ptr<DiscordRuntime> discord_;
  std::unique_ptr<VoiceGateway> voice_gateway_;
  std::unique_ptr<VoiceInputAdapter> voice_input_adapter_;
  std::unique_ptr<TranscriptionClient> transcription_;
  std::unique_ptr<TextToSpeechClient> text_to_speech_;
  std::unique_ptr<AudioNormalizer> audio_normalizer_;
  std::unique_ptr<TtsCache> tts_cache_;
  std::unique_ptr<RuntimeFeatureControlRepository> runtime_feature_controls_;
  std::unique_ptr<RetentionRepository> retention_repository_;
  std::unique_ptr<ServiceNotifier> service_notifier_;
  std::atomic<bool> service_ready_notified_{false};
  std::unique_ptr<AiResponder> ai_responder_;
  std::unique_ptr<AiWorkService> ai_work_;
  std::unique_ptr<ServerScopePolicy> scope_policy_;
  std::unique_ptr<HealthService> health_service_;
  std::unique_ptr<OwnerAdminService> owner_admin_;
  std::unique_ptr<SanguiniusOverviewService> overview_service_;
  std::unique_ptr<MessageHandler> message_handler_;
  std::unique_ptr<PendingNoticeService> notice_service_;
  std::unique_ptr<OutboxService> outbox_;
  std::unique_ptr<ChronicleService> chronicle_service_;
  std::unique_ptr<ChronicleSessionService> chronicle_session_service_;
  std::unique_ptr<RelationshipService> relationship_service_;
  std::unique_ptr<AppearanceService> appearance_service_;
  std::unique_ptr<TarotService> tarot_service_;
  std::unique_ptr<TarotWagerService> wager_service_;
  std::unique_ptr<TarotDrawService> tarot_draw_service_;
  std::unique_ptr<TarotHouseService> tarot_house_service_;
  std::unique_ptr<TarotIntegrationService> tarot_integration_service_;
  std::unique_ptr<VoxService> vox_service_;
  std::unique_ptr<SpeechService> speech_service_;
  std::unique_ptr<VoxNarrationService> vox_narration_service_;
  std::unique_ptr<VoiceListeningService> voice_listening_service_;
  std::unique_ptr<SafetyControlService> safety_controls_;
  std::unique_ptr<RetentionService> retention_service_;
  std::unique_ptr<CrossFeatureOrchestrator> cross_feature_orchestrator_;
  std::unique_ptr<ReliabilityTestService> reliability_tests_;
  std::unique_ptr<MessageObservationPipeline> message_observation_pipeline_;
  std::unique_ptr<SchedulerService> scheduler_;
  std::unique_ptr<DurableWorkControlService> durable_controls_;
  std::unique_ptr<InteractionHandler> interaction_handler_;
  std::unique_ptr<InteractionRouter> interaction_router_;
  std::mutex state_mutex_;
  ApplicationState state_{ApplicationState::created};
  bool ai_started_{false};
  bool message_handler_started_{false};
  bool interaction_handler_started_{false};
  bool outbox_started_{false};
  bool scheduler_started_{false};
  bool vox_started_{false};
  bool speech_started_{false};
  bool vox_narration_started_{false};
  bool voice_listening_started_{false};
  bool cross_feature_started_{false};
  std::int64_t last_application_heartbeat_ms_{};
  bool instance_started_{false};
  bool instance_finished_{false};
};

Application::Application(ApplicationOptions options,
                         ApplicationDependencies dependencies) {
  validate(dependencies);
  impl_ = std::make_unique<Impl>(std::move(options), std::move(dependencies));
}

Application::~Application() { stop(); }

void Application::start() { impl_->start(); }

void Application::stop() noexcept { impl_->stop(); }

} // namespace sanguinius
