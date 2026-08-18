#include "sanguinius/application.hpp"

#include "sanguinius/ai_responder.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/message_handler.hpp"
#include "sanguinius/owner_admin.hpp"

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

void validate(const ApplicationDependencies &dependencies) {
  if (!dependencies.clock || !dependencies.id_generator ||
      !dependencies.diagnostics || !dependencies.message_log ||
      !dependencies.ai_client || !dependencies.discord) {
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
        diagnostics_{std::move(dependencies.diagnostics)},
        message_log_{std::move(dependencies.message_log)},
        ai_client_{std::move(dependencies.ai_client)},
        discord_{std::move(dependencies.discord)} {
    if (!clock_ || !id_generator_ || !diagnostics_ || !message_log_ ||
        !ai_client_ || !discord_) {
      throw std::invalid_argument{
          "Application dependencies must all be configured."};
    }

    ai_responder_ = std::make_unique<AiResponder>(
        *ai_client_, *discord_, *discord_, *diagnostics_, options_.persona,
        options_.ai_queue_capacity, options_.ai_worker_count);
    scope_policy_ = std::make_unique<ServerScopePolicy>(options_.server_scope);
    health_service_ = std::make_unique<HealthService>(
        options_.build, options_.controls, options_.features);
    owner_admin_ = std::make_unique<OwnerAdminService>(
        options_.controls, *scope_policy_, *health_service_);
    message_handler_ = std::make_unique<MessageHandler>(
        *message_log_, *ai_responder_, *discord_, *diagnostics_, *owner_admin_,
        options_.command_prefix, options_.message_queue_capacity);
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
      ai_responder_->start();
      ai_started_ = true;
      message_handler_->start();
      message_handler_started_ = true;
      discord_->start([this](IncomingMessage message) {
        try {
          message.correlation_id = id_generator_->next_id();
          static_cast<void>(message_handler_->enqueue(std::move(message)));
        } catch (const std::exception &error) {
          diagnostics_->emit(
              {DiagnosticSeverity::error, "message.intake", error.what(), {}});
        } catch (...) {
          diagnostics_->emit({DiagnosticSeverity::error,
                              "message.intake",
                              "Unknown message intake failure.",
                              {}});
        }
      });
      const std::scoped_lock lock{state_mutex_};
      state_ = ApplicationState::running;
    } catch (...) {
      stop_components();
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

    const std::scoped_lock lock{state_mutex_};
    state_ = ApplicationState::stopped;
  }

private:
  void stop_components() noexcept {
    discord_->stop_accepting();
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

  ApplicationOptions options_;
  std::unique_ptr<Clock> clock_;
  std::unique_ptr<IdGenerator> id_generator_;
  std::unique_ptr<Diagnostics> diagnostics_;
  std::unique_ptr<MessageLog> message_log_;
  std::unique_ptr<AiClient> ai_client_;
  std::unique_ptr<DiscordRuntime> discord_;
  std::unique_ptr<AiResponder> ai_responder_;
  std::unique_ptr<ServerScopePolicy> scope_policy_;
  std::unique_ptr<HealthService> health_service_;
  std::unique_ptr<OwnerAdminService> owner_admin_;
  std::unique_ptr<MessageHandler> message_handler_;
  std::mutex state_mutex_;
  ApplicationState state_{ApplicationState::created};
  bool ai_started_{false};
  bool message_handler_started_{false};
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
