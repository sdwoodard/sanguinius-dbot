#include "sanguinius/application.hpp"

#include "sanguinius/ai_responder.hpp"
#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/durable_work_controls.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/interaction_handler.hpp"
#include "sanguinius/interaction_router.hpp"
#include "sanguinius/message_handler.hpp"
#include "sanguinius/outbox.hpp"
#include "sanguinius/owner_admin.hpp"
#include "sanguinius/scheduler.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::int64_t disabled_feature_job_delay_ms = 60'000;

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

void validate(const ApplicationDependencies &dependencies) {
  if (!dependencies.clock || !dependencies.id_generator ||
      !dependencies.persistent_id_generator || !dependencies.diagnostics ||
      !dependencies.message_log || !dependencies.application_instances ||
      !dependencies.identities || !dependencies.pending_notices ||
      !dependencies.durable_work || !dependencies.ai_client ||
      !dependencies.discord) {
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
        random_{std::move(dependencies.random)},
        appearance_policy_{std::move(dependencies.appearance_policy)},
        ai_client_{std::move(dependencies.ai_client)},
        discord_{std::move(dependencies.discord)},
        voice_gateway_{std::move(dependencies.voice_gateway)} {
    if (!clock_ || !id_generator_ || !persistent_id_generator_ ||
        !diagnostics_ || !message_log_ || !application_instances_ ||
        !identities_ || !pending_notices_ || !durable_work_ || !ai_client_ ||
        !discord_) {
      throw std::invalid_argument{
          "Application dependencies must all be configured."};
    }
    if (options_.features.vox_enabled &&
        (!vox_repository_ || !voice_gateway_)) {
      throw std::invalid_argument{
          "Vox persistence and the voice gateway are required when enabled."};
    }

    scope_policy_ = std::make_unique<ServerScopePolicy>(options_.server_scope);
    notice_service_ = std::make_unique<PendingNoticeService>(
        *pending_notices_, *clock_, *persistent_id_generator_);
    outbox_ = std::make_unique<OutboxService>(
        *durable_work_, *clock_, *persistent_id_generator_, *diagnostics_,
        *discord_, *discord_, options_.server_scope, options_.instance_id, 32,
        options_.durable_delivery_receipt_wait);
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
          [this] { outbox_->wake(); });
      wager_service_ = std::make_unique<TarotWagerService>(
          *wager_repository_, *clock_, *persistent_id_generator_,
          options_.wager_policy, options_.tarot_policy.starting_fate,
          options_.server_scope, options_.controls.test_mode, *diagnostics_,
          [this] {
            if (scheduler_)
              scheduler_->wake();
          },
          [this] { outbox_->wake(); });
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
            [this](const std::string_view event_type) {
              outbox_->wake();
              if (tarot_integration_service_)
                tarot_integration_service_->observe_committed_event(event_type);
            });
        tarot_draw_service_ = std::make_unique<TarotDrawService>(
            *tarot_draw_repository_, *options_.tarot_deck_catalog, *clock_,
            *persistent_id_generator_, *random_, options_.tarot_house_policy,
            options_.server_scope, options_.controls.test_mode, *diagnostics_,
            [this](const TarotDrawRecord &draw) {
              outbox_->wake();
              if (tarot_house_service_)
                tarot_house_service_->observe_draw(draw);
              if (tarot_integration_service_)
                tarot_integration_service_->observe_committed_event(
                    "tarot.draw_created.v1");
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
            if (relationship_service_) {
              try {
                static_cast<void>(relationship_service_->recover());
              } catch (const std::exception &error) {
                diagnostics_->emit({DiagnosticSeverity::error,
                                    "relationship.canon_sync",
                                    error.what(),
                                    {}});
              } catch (...) {
                diagnostics_->emit(
                    {DiagnosticSeverity::error,
                     "relationship.canon_sync",
                     "Unknown Chronicle relationship synchronization failure.",
                     {}});
              }
            }
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
          ai_work_.get(), durable_work_.get(), diagnostics_.get());
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
      vox_service_ = std::make_unique<VoxService>(
          *vox_repository_, *voice_gateway_, *clock_,
          *persistent_id_generator_, *diagnostics_, options_.server_scope,
          options_.controls, options_.instance_id,
          [this] {
            if (scheduler_)
              scheduler_->wake();
          },
          [this] { outbox_->wake(); });
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
              throw std::runtime_error{
                  "The Vox timeout queue is unavailable."};
            }
          });
    };
    add_vox_handler(vox_connect_timeout_job_type);
    add_vox_handler(vox_reconnect_timeout_job_type);
    add_vox_handler(vox_leave_timeout_job_type);
    add_vox_handler(vox_empty_timeout_job_type);
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
          if (!tarot_integration_service_ ||
              !tarot_integration_service_->enabled()) {
            const auto current = unix_milliseconds(*clock_);
            static_cast<void>(durable_work_->defer_job(
                job, current, current + disabled_feature_job_delay_ms,
                "feature_disabled"));
            return;
          }
          tarot_integration_service_->validate_scan_job(job);
          static_cast<void>(tarot_integration_service_->scan());
          const auto current = unix_milliseconds(*clock_);
          if (durable_work_->reschedule_job(job, current, current + 60'000) !=
              WorkMutationStatus::applied)
            throw std::runtime_error{
                "Tarot integration scan claim became stale."};
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
          const bool scanned =
              appearance_service_ && appearance_service_->scan_events();
          const auto current = unix_milliseconds(*clock_);
          const auto status =
              scanned
                  ? durable_work_->reschedule_job(job, current,
                                                  current + 60'000)
                  : durable_work_->defer_job(job, current, current + 60'000,
                                             "appearance_runtime_not_ready");
          if (status != WorkMutationStatus::applied)
            throw std::runtime_error{"Appearance scan claim became stale."};
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
        });
    owner_admin_ = std::make_unique<OwnerAdminService>(
        options_.controls, *scope_policy_, *health_service_);
    message_handler_ = std::make_unique<MessageHandler>(
        *message_log_, *ai_responder_, *discord_, *diagnostics_, *owner_admin_,
        options_.command_prefix, options_.message_queue_capacity,
        [this](const IncomingMessage &message) {
          const bool primary_scope =
              message.guild_id == options_.server_scope.guild_id &&
              message.channel_id == options_.server_scope.primary_channel_id;
          if (primary_scope) {
            try {
              identities_->ensure_user(DiscordUserRecord{
                  .user_id = message.author_user_id,
                  .display_name =
                      message.author_display_name.empty()
                          ? std::nullopt
                          : std::optional<
                                std::string>{message.author_display_name},
                  .username =
                      message.author_username.empty()
                          ? std::nullopt
                          : std::optional<std::string>{message.author_username},
                  .is_bot = message.author_is_bot,
                  .observed_at_ms = unix_milliseconds(*clock_),
              });
            } catch (const std::exception &error) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "message.identity_observer", error.what(),
                                  message.correlation_id});
            } catch (...) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "message.identity_observer",
                                  "Unknown identity observation failure.",
                                  message.correlation_id});
            }
          }
          if (chronicle_session_service_) {
            try {
              chronicle_session_service_->observe_message(message);
            } catch (const std::exception &error) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "chronicle.session_context_observer",
                                  error.what(), message.correlation_id});
            } catch (...) {
              diagnostics_->emit(
                  {DiagnosticSeverity::error,
                   "chronicle.session_context_observer",
                   "Unknown Chronicle session observation failure.",
                   message.correlation_id});
            }
          }
          const bool appearance_author_is_observable =
              !message.author_is_bot ||
              (message.bot_user_id.is_set() &&
               message.author_user_id == message.bot_user_id);
          const bool appearance_direct_invocation =
              !message.author_is_bot &&
              (parse_admin_operation(message.content, options_.command_prefix)
                   .has_value() ||
               ai_responder_->handles(message) ||
               parse_command(message.content, options_.command_prefix) !=
                   Command::none);
          if (primary_scope && appearance_service_ &&
              appearance_author_is_observable &&
              !appearance_direct_invocation) {
            try {
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
            } catch (const std::exception &error) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "appearance.message_observer", error.what(),
                                  message.correlation_id});
            } catch (...) {
              diagnostics_->emit({DiagnosticSeverity::error,
                                  "appearance.message_observer",
                                  "Unknown appearance observation failure.",
                                  message.correlation_id});
            }
          }
        });
    interaction_handler_ = std::make_unique<InteractionHandler>(
        *identities_, *notice_service_, *clock_, *durable_controls_,
        chronicle_service_.get(), chronicle_session_service_.get(),
        *health_service_, *diagnostics_, options_.features,
        [this] { return message_handler_->queue_snapshot(); },
        [this] { return ai_responder_->queue_snapshot(); },
        options_.interaction_queue_capacity, relationship_service_.get(),
        appearance_service_.get(), tarot_service_.get(), wager_service_.get(),
        tarot_draw_service_.get(), tarot_house_service_.get(),
        tarot_integration_service_.get(), vox_service_.get());
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
      application_instances_->record_start(ApplicationInstanceRecord{
          .instance_id = options_.instance_id,
          .application_version = options_.build.version,
          .git_revision = options_.build.revision,
          .hostname = options_.hostname,
          .process_id = options_.process_id,
          .started_at_ms = unix_milliseconds(*clock_),
      });
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
      if (tarot_house_service_)
        tarot_house_service_->reconcile_draws();
      if (tarot_house_service_ &&
          !tarot_house_service_->check_invariants().valid) {
        throw std::runtime_error{
            "House escrow invariant verification failed during startup."};
      }
      if (tarot_house_service_)
        tarot_house_service_->ensure_weekly_schedule();
      if (tarot_integration_service_)
        tarot_integration_service_->ensure_schedule();
      if (relationship_service_) {
        static_cast<void>(relationship_service_->recover());
        if (!relationship_service_->check_projection().valid) {
          throw std::runtime_error{
              "Relationship projection verification failed during startup."};
        }
      }
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
      if (vox_service_) {
        vox_service_->start();
        vox_started_ = true;
      }
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
                          options_.features.vox_enabled));
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

    stop_components();
    finish_instance(ApplicationStopReason::clean_shutdown);

    const std::scoped_lock lock{state_mutex_};
    state_ = ApplicationState::stopped;
  }

private:
  void stop_components() noexcept {
    discord_->stop_accepting();
    if (interaction_router_) {
      interaction_router_->stop();
    }
    if (scheduler_started_) {
      scheduler_->stop();
      scheduler_started_ = false;
    }
    if (vox_started_) {
      vox_service_->stop();
      vox_started_ = false;
    }
    if (outbox_started_) {
      outbox_->stop();
      outbox_started_ = false;
    }
    if (interaction_handler_started_) {
      interaction_handler_->stop();
      interaction_handler_started_ = false;
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
  std::unique_ptr<Random> random_;
  std::optional<AppearancePolicy> appearance_policy_;
  std::unique_ptr<AiClient> ai_client_;
  std::unique_ptr<DiscordRuntime> discord_;
  std::unique_ptr<VoiceGateway> voice_gateway_;
  std::unique_ptr<AiResponder> ai_responder_;
  std::unique_ptr<AiWorkService> ai_work_;
  std::unique_ptr<ServerScopePolicy> scope_policy_;
  std::unique_ptr<HealthService> health_service_;
  std::unique_ptr<OwnerAdminService> owner_admin_;
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
