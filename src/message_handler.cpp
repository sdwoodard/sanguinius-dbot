#include "sanguinius/message_handler.hpp"

#include <utility>

namespace sanguinius {
namespace {

constexpr std::string_view overload_message{
    "I am handling too many requests right now. Please try again shortly."};

[[nodiscard]] MessageReference target(const IncomingMessage &message) {
  return MessageReference{message.message_id, message.guild_id,
                          message.channel_id};
}

[[nodiscard]] ServerRequestContext
server_context(const IncomingMessage &message) {
  return ServerRequestContext{message.guild_id, message.channel_id,
                              message.author_user_id};
}

} // namespace

MessageHandler::MessageHandler(
    MessageLog &message_log, AiResponder &ai_responder,
    DiscordTextDelivery &delivery, Diagnostics &diagnostics,
    OwnerAdminService &owner_admin, std::string command_prefix,
    const std::size_t queue_capacity,
    std::function<void(const IncomingMessage &)> observer,
    std::function<std::string(const HealthSnapshot &)> health_renderer)
    : message_log_{message_log}, ai_responder_{ai_responder},
      delivery_{delivery}, diagnostics_{diagnostics}, owner_admin_{owner_admin},
      command_prefix_{std::move(command_prefix)},
      observer_{std::move(observer)},
      health_renderer_{std::move(health_renderer)}, worker_{queue_capacity, 1} {
  if (!health_renderer_)
    health_renderer_ = [](const HealthSnapshot &snapshot) {
      return render_health(snapshot);
    };
}

MessageHandler::~MessageHandler() { stop(); }

void MessageHandler::start() { worker_.start(); }

void MessageHandler::stop() noexcept { worker_.stop(); }

SubmitResult MessageHandler::enqueue(IncomingMessage message) {
  const bool should_notify = actionable(message);
  const auto result =
      worker_.try_submit([this, message](const std::stop_token stop_token) {
        if (stop_token.stop_requested()) {
          return;
        }
        try {
          process(message);
        } catch (const std::exception &error) {
          diagnostics_.emit({DiagnosticSeverity::error, "message.handling",
                             error.what(), message.correlation_id});
        }
      });

  if (result == SubmitResult::full) {
    diagnostics_.emit({DiagnosticSeverity::warning, "message.queue_full",
                       "Incoming message was rejected because the application "
                       "queue is full.",
                       message.correlation_id});
    if (should_notify) {
      send_overload(message);
    }
  }
  return result;
}

QueueSnapshot MessageHandler::queue_snapshot() const {
  return worker_.snapshot();
}

void MessageHandler::process(const IncomingMessage &message) const {
  try {
    message_log_.append({message.author_username, message.content});
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "message.logging",
                       error.what(), message.correlation_id});
  }

  if (observer_) {
    try {
      observer_(message);
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "message.observer",
                         error.what(), message.correlation_id});
    }
  }

  if (message.author_is_bot) {
    return;
  }

  if (const auto operation =
          parse_admin_operation(message.content, command_prefix_);
      operation.has_value()) {
    const auto result =
        owner_admin_.handle(AdminRequest{server_context(message), *operation},
                            worker_.snapshot(), ai_responder_.queue_snapshot());
    if (result.authorization.allowed() && result.health.has_value()) {
      send_health(message, *result.health);
    } else {
      diagnostics_.emit(
          {DiagnosticSeverity::info, "admin.request_rejected",
           std::string{"Owner admin request was not handled: status="} +
               admin_status_name(result.authorization.status) + " scope=" +
               scope_rejection_name(result.authorization.rejection) + ".",
           message.correlation_id});
    }
    return;
  }

  if (ai_responder_.handles(message)) {
    if (ai_responder_.enqueue(message) == SubmitResult::full) {
      send_overload(message);
    }
    return;
  }
}

void MessageHandler::send_health(const IncomingMessage &message,
                                 const HealthSnapshot &snapshot) const {
  delivery_.reply({
      .target = target(message),
      .content = health_renderer_(snapshot),
      .embed = std::nullopt,
      .suppress_mentions = true,
  });
}

void MessageHandler::send_overload(
    const IncomingMessage &message) const noexcept {
  try {
    delivery_.reply({
        .target = target(message),
        .content = std::string{overload_message},
        .embed = std::nullopt,
        .suppress_mentions = true,
    });
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "discord.delivery",
                       error.what(), message.correlation_id});
  }
}

bool MessageHandler::actionable(const IncomingMessage &message) const {
  if (message.author_is_bot) {
    return false;
  }
  if (parse_admin_operation(message.content, command_prefix_).has_value()) {
    return owner_admin_.authorize(server_context(message)).allowed();
  }
  return ai_responder_.handles(message);
}

} // namespace sanguinius
