#include "sanguinius/ai_responder.hpp"

#include <algorithm>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::size_t history_size = 8;
constexpr std::size_t maximum_discord_reply_size = 1'900;

[[nodiscard]] std::string limited(const std::string_view text,
                                  const std::size_t maximum_size) {
  if (text.size() <= maximum_size) {
    return std::string{text};
  }

  std::size_t end = maximum_size;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0b1100'0000U) ==
                        0b1000'0000U) {
    --end;
  }
  return std::string{text.substr(0, end)} + "…";
}

[[nodiscard]] ReplyRequest reply_to(const IncomingMessage &message,
                                    std::string content) {
  return ReplyRequest{
      .target = MessageReference{message.message_id, message.guild_id,
                                 message.channel_id},
      .content = std::move(content),
      .embed = std::nullopt,
      .suppress_mentions = true,
  };
}

} // namespace

std::optional<std::string> prompt_after_bot_mention(std::string_view content,
                                                    const DiscordId bot_id) {
  const auto first = content.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  content.remove_prefix(first);

  const auto bot_id_text = bot_id.str();
  const std::string mention = "<@" + bot_id_text + '>';
  const std::string nickname_mention = "<@!" + bot_id_text + '>';
  std::size_t mention_size = 0;
  if (content.starts_with(mention)) {
    mention_size = mention.size();
  } else if (content.starts_with(nickname_mention)) {
    mention_size = nickname_mention.size();
  } else {
    return std::nullopt;
  }

  content.remove_prefix(mention_size);
  const auto prompt_start = content.find_first_not_of(" \t\r\n,:-");
  if (prompt_start == std::string_view::npos) {
    return std::string{};
  }
  return std::string{content.substr(prompt_start)};
}

AiResponder::AiResponder(const AiClient &client,
                         DiscordConversation &conversation,
                         DiscordTextDelivery &delivery,
                         Diagnostics &diagnostics, std::string persona,
                         const std::size_t queue_capacity,
                         const std::size_t worker_count,
                         RelationshipService *relationships,
                         const FeatureConfiguration features)
    : client_{client}, conversation_{conversation}, delivery_{delivery},
      diagnostics_{diagnostics}, compiler_{std::move(persona)},
      relationships_{relationships}, features_{features},
      workers_{queue_capacity, worker_count} {}

AiResponder::~AiResponder() { stop(); }

void AiResponder::start() { workers_.start(); }

void AiResponder::stop() noexcept { workers_.stop(); }

bool AiResponder::handles(const IncomingMessage &message) const {
  return !message.author_is_bot && message.bot_user_id != 0 &&
         prompt_after_bot_mention(message.content, message.bot_user_id)
             .has_value();
}

SubmitResult AiResponder::enqueue(IncomingMessage message) {
  return workers_.try_submit([this, message = std::move(message)](
                                 const std::stop_token stop_token) {
    try {
      respond(message, stop_token);
    } catch (const OperationCancelled &) {
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "ai.response", error.what(),
                         message.correlation_id});
      if (stop_token.stop_requested()) {
        return;
      }
      try {
        delivery_.reply(reply_to(
            message, "I could not form a response just now. Please try again "
                     "shortly."));
      } catch (const std::exception &delivery_error) {
        diagnostics_.emit({DiagnosticSeverity::error, "discord.delivery",
                           delivery_error.what(), message.correlation_id});
      }
    }
  });
}

QueueSnapshot AiResponder::queue_snapshot() const {
  return workers_.snapshot();
}

void AiResponder::respond(const IncomingMessage &message,
                          const std::stop_token stop_token) const {
  if (stop_token.stop_requested()) {
    throw OperationCancelled{};
  }

  delivery_.show_typing(message.channel_id);
  std::vector<ConversationEntry> recent;
  try {
    recent = conversation_.recent_messages(message, history_size, stop_token);
  } catch (const OperationCancelled &) {
    throw;
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::warning, "discord.history",
                       error.what(), message.correlation_id});
  }

  std::optional<ConversationEntry> replied;
  try {
    replied = replied_message(message, recent, stop_token);
  } catch (const OperationCancelled &) {
    throw;
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::warning, "discord.reply_context",
                       error.what(), message.correlation_id});
  }

  const auto current_request =
      prompt_after_bot_mention(message.content, message.bot_user_id).value_or("");
  PreparedPromptContext social;
  if (relationships_ != nullptr) {
    social = relationships_->prepare_prompt(
        message, current_request, replied ? replied->content : std::string{});
    if (social.status == PromptPreparationStatus::duplicate) return;
  }
  const auto fail_attempt =
      [this, &message, &social](const std::string_view outcome,
                               const std::string_view error_code) noexcept {
        if (!social.attempt_id || relationships_ == nullptr) return;
        try {
          static_cast<void>(relationships_->fail_prompt(
              *social.attempt_id, outcome, error_code));
        } catch (const std::exception &error) {
          diagnostics_.emit({DiagnosticSeverity::error, "ai.prompt_attempt",
                             error.what(), message.correlation_id});
        } catch (...) {
          diagnostics_.emit(
              {DiagnosticSeverity::error, "ai.prompt_attempt",
               "Unknown prompt-attempt finalization failure.",
               message.correlation_id});
        }
      };
  AiRequest request;
  try {
    request = compiler_.compile(PromptCompilerInput{
        .message = message,
        .current_request = current_request,
        .recent = std::move(recent),
        .replied = replied,
        .social = social,
        .features = features_,
    });
  } catch (...) {
    fail_attempt("model_failed", "compilation_failed");
    throw;
  }
  std::string response;
  try {
    response = client_.generate(request, stop_token);
  } catch (const OperationCancelled &) {
    fail_attempt("cancelled", "shutdown");
    throw;
  } catch (...) {
    fail_attempt("model_failed", "generation_failed");
    throw;
  }
  if (stop_token.stop_requested()) {
    fail_attempt("cancelled", "shutdown");
    throw OperationCancelled{};
  }
  if (social.attempt_id && relationships_ != nullptr) {
    PromptFinalizationStatus finalized;
    try {
      finalized = relationships_->complete_prompt(*social.attempt_id);
    } catch (...) {
      fail_attempt("model_failed", "finalization_failed");
      throw;
    }
    if (finalized != PromptFinalizationStatus::applied) return;
  }
  response = limited(response, maximum_discord_reply_size);
  delivery_.reply(reply_to(message, std::move(response)));
}

std::optional<ConversationEntry>
AiResponder::replied_message(const IncomingMessage &message,
                             const std::vector<ConversationEntry> &recent,
                             const std::stop_token stop_token) const {
  if (!message.replied_to.has_value() || message.replied_to->message_id == 0) {
    return std::nullopt;
  }

  const auto reply_id = message.replied_to->message_id;
  if (const auto found =
          std::ranges::find(recent, reply_id, &ConversationEntry::message_id);
      found != recent.end()) {
    return *found;
  }

  const auto channel_id = message.replied_to->channel_id == 0
                              ? message.channel_id
                              : message.replied_to->channel_id;
  return conversation_.message(reply_id, channel_id, stop_token);
}

} // namespace sanguinius
