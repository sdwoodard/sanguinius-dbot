#include "sanguinius/application.hpp"

#include "sanguinius/ai_responder.hpp"
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
        ai_client_{std::move(dependencies.ai_client)},
        discord_{std::move(dependencies.discord)} {
    if (!clock_ || !id_generator_ || !persistent_id_generator_ ||
        !diagnostics_ || !message_log_ || !application_instances_ ||
        !identities_ || !pending_notices_ || !durable_work_ || !ai_client_ ||
        !discord_) {
      throw std::invalid_argument{
          "Application dependencies must all be configured."};
    }

    ai_responder_ = std::make_unique<AiResponder>(
        *ai_client_, *discord_, *discord_, *diagnostics_, options_.persona,
        options_.ai_queue_capacity, options_.ai_worker_count);
    scope_policy_ = std::make_unique<ServerScopePolicy>(options_.server_scope);
    notice_service_ = std::make_unique<PendingNoticeService>(
        *pending_notices_, *clock_, *persistent_id_generator_);
    outbox_ = std::make_unique<OutboxService>(
        *durable_work_, *clock_, *persistent_id_generator_, *diagnostics_,
        *discord_, *discord_, options_.server_scope, options_.instance_id, 32,
        options_.durable_delivery_receipt_wait);
    scheduler_ = std::make_unique<SchedulerService>(
        *durable_work_, *clock_, *persistent_id_generator_, *diagnostics_,
        options_.instance_id, [this] { outbox_->wake(); });
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
        });
    owner_admin_ = std::make_unique<OwnerAdminService>(
        options_.controls, *scope_policy_, *health_service_);
    message_handler_ = std::make_unique<MessageHandler>(
        *message_log_, *ai_responder_, *discord_, *diagnostics_, *owner_admin_,
        options_.command_prefix, options_.message_queue_capacity);
    interaction_handler_ = std::make_unique<InteractionHandler>(
        *identities_, *notice_service_, *clock_, *durable_controls_,
        *health_service_, *diagnostics_, options_.features,
        [this] { return message_handler_->queue_snapshot(); },
        [this] { return ai_responder_->queue_snapshot(); },
        options_.interaction_queue_capacity);
    interaction_router_ = std::make_unique<InteractionRouter>(
        *scope_policy_, options_.controls, *interaction_handler_,
        *diagnostics_);
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
      static_cast<void>(notice_service_->recover_incomplete_deliveries());
      outbox_->start();
      outbox_started_ = true;
      scheduler_->start();
      scheduler_started_ = true;
      ai_responder_->start();
      ai_started_ = true;
      message_handler_->start();
      message_handler_started_ = true;
      interaction_handler_->start();
      interaction_handler_started_ = true;
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
          command_catalog(options_.controls.admin_commands_enabled));
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
  std::unique_ptr<AiClient> ai_client_;
  std::unique_ptr<DiscordRuntime> discord_;
  std::unique_ptr<AiResponder> ai_responder_;
  std::unique_ptr<ServerScopePolicy> scope_policy_;
  std::unique_ptr<HealthService> health_service_;
  std::unique_ptr<OwnerAdminService> owner_admin_;
  std::unique_ptr<MessageHandler> message_handler_;
  std::unique_ptr<PendingNoticeService> notice_service_;
  std::unique_ptr<OutboxService> outbox_;
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
