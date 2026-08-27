#include "sanguinius/interaction_handler.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

struct DeliveryNotifier {
  explicit DeliveryNotifier(DeliveryCallback value)
      : callback{std::move(value)} {}

  void notify(const DeliveryResult result) noexcept {
    try {
      std::call_once(once, [this, result] {
        auto current = std::move(callback);
        if (current) {
          try {
            current(result);
          } catch (...) {
          }
        }
      });
    } catch (...) {
    }
  }

  DeliveryCallback callback;
  std::once_flag once;
};

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
               const std::size_t pending_notices,
               const std::string_view appearance_status,
               const bool appearance_callbacks_enabled) {
  std::ostringstream output;
  output << "Sanguinius is ready.\n"
         << "Unopened sealed notices: " << pending_notices << "\n"
         << "Chronicle: " << enabled(features.chronicle_enabled) << "\n"
         << "Tarot: " << enabled(features.tarot_enabled) << "\n"
         << "Appearances: " << appearance_mode_name(features.appearances_mode)
         << "\nYour appearance callbacks: "
         << enabled(appearance_callbacks_enabled)
         << "\nAppearance controls: " << appearance_status << "\n"
         << "Vox: " << enabled(features.vox_enabled);
  return text_message(output.str());
}

[[nodiscard]] InteractionMessage privacy_message(
    const FeatureConfiguration &features, const UserPreferences &preferences,
    const std::size_t pending_notices, const std::string_view appearance_status,
    const std::string_view tarot_status) {
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
         << "\nServer-wide appearance controls: " << appearance_status
         << "\nVoice input globally: " << enabled(features.voice_input_enabled)
         << "\nVoice-input consent rule: guild-wide prior consent must be "
            "attested by the owner; the legacy personal preference is not a "
            "capture gate"
         << "\nUnopened sealed notices: " << pending_notices << "\n"
         << tarot_status
         << "\nDiscord DMs are never used. Raw received voice audio is never "
            "persisted.";
  return text_message(output.str());
}

} // namespace

namespace interaction_handler_detail {

SequencedInteractionEditor::SequencedInteractionEditor(Sender sender)
    : sender_{std::move(sender)} {
  if (!sender_)
    throw std::invalid_argument{
        "A sequenced interaction editor requires a sender."};
}

void SequencedInteractionEditor::submit(InteractionMessage message,
                                        DeliveryCallback callback,
                                        const bool terminal) noexcept {
  std::optional<PendingEdit> dispatch_now;
  DeliveryCallback rejected;
  {
    const std::scoped_lock lock{mutex_};
    PendingEdit pending{std::move(message), std::move(callback)};
    if (terminal) {
      terminal_seen_ = true;
      if (in_flight_) {
        if (pending_terminal_)
          rejected = std::move(pending_terminal_->callback);
        pending_terminal_ = std::move(pending);
      } else {
        in_flight_ = true;
        dispatch_now = std::move(pending);
      }
    } else if (in_flight_ || terminal_seen_) {
      rejected = std::move(pending.callback);
    } else {
      in_flight_ = true;
      dispatch_now = std::move(pending);
    }
  }
  if (rejected) {
    try {
      rejected(DeliveryResult::permanent_failure);
    } catch (...) {
    }
  }
  if (dispatch_now)
    dispatch(std::move(*dispatch_now));
}

void SequencedInteractionEditor::dispatch(PendingEdit pending) noexcept {
  const auto self = shared_from_this();
  const auto once = std::make_shared<std::once_flag>();
  auto completion = [self, once, callback = std::move(pending.callback)](
                        const DeliveryResult result) mutable noexcept {
    try {
      std::call_once(*once, [&] {
        if (callback) {
          try {
            callback(result);
          } catch (...) {
          }
        }
        self->complete();
      });
    } catch (...) {
    }
  };
  try {
    sender_(std::move(pending.message), completion);
  } catch (...) {
    completion(DeliveryResult::unknown_outcome);
  }
}

void SequencedInteractionEditor::complete() noexcept {
  std::optional<PendingEdit> next;
  {
    const std::scoped_lock lock{mutex_};
    if (pending_terminal_) {
      next = std::move(pending_terminal_);
      pending_terminal_.reset();
    } else {
      in_flight_ = false;
    }
  }
  if (next)
    dispatch(std::move(*next));
}

} // namespace interaction_handler_detail

InteractionHandler::InteractionHandler(
    CoreIdentityRepository &identities, PendingNoticeService &notices,
    const Clock &clock, DurableWorkControlService &durable_controls,
    ChronicleService *chronicle, ChronicleSessionService *chronicle_sessions,
    HealthService &health_service, Diagnostics &diagnostics,
    const FeatureConfiguration features,
    std::function<QueueSnapshot()> message_queue,
    std::function<QueueSnapshot()> ai_queue, const std::size_t queue_capacity,
    RelationshipService *relationships, AppearanceService *appearances,
    TarotService *tarot, TarotWagerService *wagers,
    TarotDrawService *tarot_draws, TarotHouseService *tarot_house,
    TarotIntegrationService *tarot_integration, VoxService *vox,
    VoxNarrationService *vox_narration, VoiceListeningService *voice_listening)
    : identities_{identities}, notices_{notices}, clock_{clock},
      durable_controls_{durable_controls}, chronicle_{chronicle},
      chronicle_sessions_{chronicle_sessions}, relationships_{relationships},
      appearances_{appearances}, tarot_{tarot}, wagers_{wagers},
      tarot_draws_{tarot_draws}, tarot_house_{tarot_house},
      tarot_integration_{tarot_integration}, vox_{vox},
      vox_narration_{vox_narration}, voice_listening_{voice_listening},
      health_service_{health_service}, diagnostics_{diagnostics},
      features_{features}, message_queue_{std::move(message_queue)},
      ai_queue_{std::move(ai_queue)},
      callbacks_{std::make_shared<CallbackFence>()}, worker_{queue_capacity, 1},
      privacy_worker_{queue_capacity, 1},
      kill_switch_worker_{queue_capacity, 1} {
  if (!message_queue_ || !ai_queue_) {
    throw std::invalid_argument{"Interaction queue observers are required."};
  }
}

InteractionHandler::~InteractionHandler() { stop(); }

void InteractionHandler::start() {
  kill_switch_worker_.start();
  privacy_worker_.start();
  worker_.start();
}

void InteractionHandler::drain() noexcept {
  // VoiceListeningService independently persists preempted emergency disables.
  // These queues therefore only finish command processing and response
  // delivery while their dependencies remain alive.
  kill_switch_worker_.stop();
  privacy_worker_.stop();
  worker_.stop();
}

void InteractionHandler::close_delivery() noexcept {
  callbacks_->close_and_wait();
}

void InteractionHandler::stop() noexcept {
  drain();
  close_delivery();
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

SubmitResult
InteractionHandler::enqueue_privacy_control(RoutedInteraction interaction) {
  auto responder = interaction.interaction.responder;
  auto correlation_id = interaction.interaction.correlation_id;
  const auto disable =
      interaction.operation == InteractionOperation::vox_listening_disable;
  const auto kill_switch_control =
      disable ||
      interaction.operation == InteractionOperation::vox_listening_enable;
  auto request = std::make_shared<RoutedInteraction>(std::move(interaction));
  const auto execute = [this, request] { process_privacy_control(*request); };
  const auto superseded = [responder] {
    if (responder) {
      responder->edit_original(text_message(
          "This voice privacy control was superseded by a newer emergency "
          "disable."));
    }
  };
  SubmitResult result{SubmitResult::stopping};
  if (disable) {
    result = kill_switch_worker_.try_submit_front_superseding(
        [execute](std::stop_token) { execute(); }, superseded);
  } else if (kill_switch_control) {
    result = kill_switch_worker_.try_submit(
        [execute](const std::stop_token token) {
          if (!token.stop_requested())
            execute();
        },
        superseded);
  } else {
    result = privacy_worker_.try_submit_front(
        [execute](const std::stop_token token) {
          if (!token.stop_requested())
            execute();
        });
  }
  if (result == SubmitResult::full) {
    diagnostics_.emit(
        {DiagnosticSeverity::warning, "interaction.privacy_queue_full",
         "A voice privacy control arrived while the dedicated privacy queue "
         "was full; previously accepted privacy controls remain prioritized.",
         correlation_id});
    if (responder) {
      responder->edit_original(
          text_message("A voice privacy control is already being processed."));
    }
  }
  return result;
}

void InteractionHandler::process_privacy_control(
    const RoutedInteraction &request) noexcept {
  try {
    process(request);
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "interaction.privacy_control",
                       error.what(), request.interaction.correlation_id});
    edit(request.interaction,
         text_message("I could not complete that voice privacy control. "
                      "Please try again immediately."),
         "interaction.response");
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error, "interaction.privacy_control",
                       "Unknown voice privacy-control failure.",
                       request.interaction.correlation_id});
    edit(request.interaction,
         text_message("I could not complete that voice privacy control. "
                      "Please try again immediately."),
         "interaction.response");
  }
}

void InteractionHandler::preempt_voice_privacy_control(
    RoutedInteraction &request) noexcept {
  if (!voice_listening_ ||
      (request.operation != InteractionOperation::vox_listen_stop &&
       request.operation != InteractionOperation::vox_listening_disable &&
       request.operation != InteractionOperation::vox_listening_enable))
    return;
  try {
    if (request.operation == InteractionOperation::vox_listening_enable) {
      request.voice_disable_generation = voice_listening_->disable_generation();
      return;
    }
    const auto window =
        request.operation == InteractionOperation::vox_listen_stop
            ? parse_voice_control(request.interaction.custom_id,
                                  voice_listening_stop_prefix)
            : std::nullopt;
    request.voice_disable_generation = voice_listening_->preempt_privacy_abort(
        request.interaction.user_id,
        request.operation == InteractionOperation::vox_listening_disable,
        window);
  } catch (...) {
  }
}

QueueSnapshot InteractionHandler::queue_snapshot() const {
  return worker_.snapshot();
}

QueueSnapshot InteractionHandler::privacy_queue_snapshot() const {
  return privacy_worker_.snapshot();
}

bool InteractionHandler::show_voice_transcript_modal(
    const IncomingInteraction &interaction) const {
  if (!voice_listening_ || !interaction.responder)
    return false;
  const auto modal = voice_listening_->transcript_modal(interaction);
  if (!modal)
    return false;
  auto receipt_context = interaction;
  receipt_context.responder.reset();
  try {
    interaction.responder->show_modal(*modal, [this, callbacks = callbacks_,
                                               receipt_context =
                                                   std::move(receipt_context)](
                                                  const DeliveryResult result) {
      try {
        static_cast<void>(callbacks->invoke([this, &receipt_context, result] {
          if (voice_listening_)
            voice_listening_->transcript_modal_delivery(receipt_context,
                                                        result);
        }));
      } catch (...) {
      }
    });
  } catch (...) {
    voice_listening_->transcript_modal_delivery(
        interaction, DeliveryResult::unknown_outcome);
    return false;
  }
  return true;
}

void InteractionHandler::process(const RoutedInteraction &request) {
  ensure_user(request.interaction);
  switch (request.operation) {
  case InteractionOperation::status: {
    const auto preferences =
        identities_.load_preferences(request.interaction.user_id);
    if (!preferences.has_value())
      throw std::runtime_error{"User preferences were not initialized."};
    edit(request.interaction,
         status_message(features_,
                        notices_.pending_count(request.interaction.user_id),
                        appearances_ ? appearances_->member_status_summary()
                                     : "unavailable",
                        preferences->appearance_callback_opt_in),
         "interaction.status");
    return;
  }
  case InteractionOperation::privacy: {
    const auto preferences =
        identities_.load_preferences(request.interaction.user_id);
    if (!preferences.has_value()) {
      throw std::runtime_error{"User preferences were not initialized."};
    }
    edit(request.interaction,
         privacy_message(
             features_, *preferences,
             notices_.pending_count(request.interaction.user_id),
             appearances_ ? appearances_->member_status_summary()
                          : "unavailable",
             tarot_
                 ? tarot_->privacy_summary(request.interaction.user_id)
                 : std::string{"Fate standings: "} +
                       (preferences->public_tarot_results_opt_in ? "public"
                                                                 : "private") +
                       "\nFate feature: disabled\nFate ledger: "
                       "retained as an immutable financial "
                       "audit; balances are derived from "
                       "postings."),
         "interaction.privacy");
    return;
  }
  case InteractionOperation::appearance_callbacks: {
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    const auto found = std::ranges::find(request.interaction.command_options,
                                         "mode", &InteractionOption::name);
    if (found == request.interaction.command_options.end())
      throw std::invalid_argument{"Appearance callback mode is required."};
    const auto *mode = std::get_if<std::string>(&found->value);
    if (mode == nullptr || (*mode != "on" && *mode != "off"))
      throw std::invalid_argument{"Appearance callback mode is invalid."};
    edit(request.interaction,
         text_message(appearances_->set_callback_consent(
             request.interaction.user_id, *mode == "on",
             "appearance.callbacks:" + request.interaction.interaction_id.str(),
             request.interaction.correlation_id)),
         "interaction.appearance_callbacks");
    return;
  }
  case InteractionOperation::appearance_quiet_for:
  case InteractionOperation::appearance_quiet_tonight:
  case InteractionOperation::appearance_quiet_until:
  case InteractionOperation::appearance_quiet_off: {
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    std::string kind;
    std::string local_time;
    std::string request_value;
    if (request.operation == InteractionOperation::appearance_quiet_for) {
      kind = "duration";
      const auto found =
          std::ranges::find(request.interaction.command_options, "duration",
                            &InteractionOption::name);
      if (found == request.interaction.command_options.end() ||
          std::get_if<std::string>(&found->value) == nullptr ||
          *std::get_if<std::string>(&found->value) != "2h")
        throw std::invalid_argument{"The quiet duration is invalid."};
      request_value = "2h";
    } else if (request.operation ==
               InteractionOperation::appearance_quiet_tonight)
      kind = "tonight";
    else if (request.operation ==
             InteractionOperation::appearance_quiet_until) {
      kind = "until";
      const auto found = std::ranges::find(request.interaction.command_options,
                                           "time", &InteractionOption::name);
      if (found == request.interaction.command_options.end() ||
          std::get_if<std::string>(&found->value) == nullptr)
        throw std::invalid_argument{"A local HH:MM time is required."};
      local_time = *std::get_if<std::string>(&found->value);
      request_value = local_time;
    }
    std::optional<std::int64_t> deadline;
    if (request.operation != InteractionOperation::appearance_quiet_off) {
      deadline = appearances_->quiet_deadline(kind, local_time);
      if (!deadline) {
        edit(request.interaction,
             text_message("That local quiet deadline is invalid or does not "
                          "exist because of daylight saving time."),
             "interaction.appearance_quiet");
        return;
      }
    }
    edit(request.interaction,
         text_message(appearances_->set_quiet(
             request.interaction.user_id, deadline, kind, request_value,
             "appearance.quiet:" + request.interaction.interaction_id.str(),
             request.interaction.correlation_id)),
         "interaction.appearance_quiet");
    return;
  }
  case InteractionOperation::appearance_feedback:
  case InteractionOperation::appearance_feedback_component: {
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    AppearanceFeedbackMutation feedback{
        .actor_user_id = request.interaction.user_id,
        .guild_id = request.interaction.guild_id,
        .channel_id = request.interaction.channel_id,
        .action = AppearanceFeedbackAction::more,
        .control_id = std::nullopt,
        .reference = std::nullopt,
        .quiet_until_ms = std::nullopt,
        .feedback_id = {},
        .event_id = {},
        .idempotency_key =
            "appearance.feedback:" + request.interaction.interaction_id.str(),
        .correlation_id = request.interaction.correlation_id};
    if (request.operation ==
        InteractionOperation::appearance_feedback_component) {
      feedback.control_id = request.interaction.custom_id.substr(
          appearance_feedback_component_prefix.size());
    } else {
      bool response_found = false;
      for (const auto &option : request.interaction.command_options) {
        const auto *value = std::get_if<std::string>(&option.value);
        if (value == nullptr)
          continue;
        if (option.name == "response") {
          const auto parsed = parse_appearance_feedback_action(*value);
          if (!parsed || *parsed == AppearanceFeedbackAction::quiet_tonight)
            throw std::invalid_argument{"Appearance feedback is invalid."};
          feedback.action = *parsed;
          response_found = true;
        } else if (option.name == "reference") {
          feedback.reference = *value;
        }
      }
      if (!response_found)
        throw std::invalid_argument{"Appearance feedback is required."};
    }
    edit(request.interaction, text_message(appearances_->feedback(feedback)),
         "interaction.appearance_feedback");
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
    if (!chronicle_)
      throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         render_chronicle_proposal(
             chronicle_->canonize_message(request.interaction)),
         "interaction.chronicle_canonize");
    return;
  case InteractionOperation::chronicle_memory_preview:
    if (!chronicle_)
      throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         chronicle_->begin_memory_preview(request.interaction),
         "interaction.chronicle_memory_preview");
    return;
  case InteractionOperation::chronicle_recall:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle search service is unavailable."};
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
    edit(request.interaction,
         chronicle_sessions_->timeline(request.interaction),
         "interaction.chronicle_timeline");
    return;
  case InteractionOperation::chronicle_forget:
    if (!chronicle_)
      throw std::runtime_error{"Chronicle service is unavailable."};
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
    if (!chronicle_)
      throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         render_chronicle_mutation(
             chronicle_->edit_proposal(request.interaction)),
         "interaction.chronicle_edit");
    return;
  case InteractionOperation::chronicle_component:
    if (!chronicle_)
      throw std::runtime_error{"Chronicle service is unavailable."};
    edit(request.interaction,
         render_chronicle_mutation(
             chronicle_->apply_component(request.interaction)),
         "interaction.chronicle_component");
    return;
  case InteractionOperation::chronicle_session_start: {
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    const auto result = chronicle_sessions_->start(request.interaction);
    edit(request.interaction,
         text_message(
             result.code == ChronicleSessionResultCode::created
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
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction, chronicle_sessions_->status(request.interaction),
         "interaction.chronicle_session_status");
    return;
  case InteractionOperation::chronicle_session_close: {
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    const auto result = chronicle_sessions_->close(request.interaction);
    edit(request.interaction,
         text_message(
             result.code == ChronicleSessionResultCode::updated
                 ? (result.wake_scheduler
                        ? "The session is closed; its chapter draft is being "
                          "prepared."
                        : "The empty session was abandoned without a chapter.")
             : result.code == ChronicleSessionResultCode::unauthorized
                 ? "Only the opener or owner may close this session."
                 : "The Chronicle session could not be closed."),
         "interaction.chronicle_session_close");
    return;
  }
  case InteractionOperation::chronicle_summary_edit:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->edit_summary(request.interaction),
         "interaction.chronicle_summary_edit");
    return;
  case InteractionOperation::chronicle_summary_approve:
  case InteractionOperation::chronicle_summary_reject:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->decide_summary(
             request.interaction,
             request.operation ==
                 InteractionOperation::chronicle_summary_approve),
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
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->propose_title(request.interaction),
         "interaction.chronicle_title_propose");
    return;
  case InteractionOperation::chronicle_title_list:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->list_titles(request.interaction),
         "interaction.chronicle_title_list");
    return;
  case InteractionOperation::chronicle_title_approve:
  case InteractionOperation::chronicle_title_reject:
  case InteractionOperation::chronicle_title_feature:
  case InteractionOperation::chronicle_title_revoke: {
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
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
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         chronicle_sessions_->set_anniversaries(
             request.interaction,
             request.operation ==
                 InteractionOperation::chronicle_anniversaries_on),
         "interaction.chronicle_anniversary_preference");
    return;
  case InteractionOperation::test_anniversary:
    if (!chronicle_sessions_)
      throw std::runtime_error{"Chronicle session service is unavailable."};
    edit(request.interaction,
         text_message(
             chronicle_sessions_->queue_test_anniversary(request.interaction)
                 ? "A test anniversary scan is queued."
                 : "That test anniversary scan was already queued or is "
                   "unavailable."),
         "interaction.test_anniversary");
    return;
  case InteractionOperation::admin_health: {
    const auto snapshot =
        health_service_.snapshot(message_queue_(), ai_queue_(), true);
    auto rendered = render_health(snapshot);
    if (appearances_)
      rendered += "\nAppearances: " + appearances_->status_summary();
    edit(request.interaction,
         text_message(bounded_health_message(std::move(rendered))),
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
  case InteractionOperation::appearance_simulate: {
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    const auto found = std::ranges::find(request.interaction.command_options,
                                         "fixture", &InteractionOption::name);
    if (found == request.interaction.command_options.end())
      throw std::invalid_argument{"Appearance fixture is required."};
    const auto *fixture = std::get_if<std::string>(&found->value);
    if (fixture == nullptr)
      throw std::invalid_argument{"Appearance fixture is invalid."};
    const auto reference = appearances_->simulate(AppearanceSimulationRequest{
        .fixture = *fixture,
        .idempotency_key =
            "appearance.simulate:" + request.interaction.interaction_id.str(),
        .correlation_id = request.interaction.correlation_id,
        .owner_user_id = request.interaction.user_id,
        .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock_.now().time_since_epoch())
                      .count(),
        .candidate_id = {},
        .event_id = {}});
    edit(request.interaction,
         text_message("Appearance simulation stored: " + reference),
         "interaction.appearance_simulate");
    return;
  }
  case InteractionOperation::appearance_preview: {
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    const auto found = std::ranges::find(request.interaction.command_options,
                                         "reference", &InteractionOption::name);
    if (found == request.interaction.command_options.end())
      throw std::invalid_argument{"Appearance reference is required."};
    const auto *reference = std::get_if<std::string>(&found->value);
    if (reference == nullptr)
      throw std::invalid_argument{"Appearance reference is invalid."};
    edit(request.interaction, text_message(appearances_->preview(*reference)),
         "interaction.appearance_preview");
    return;
  }
  case InteractionOperation::appearance_recent:
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    edit(request.interaction, text_message(appearances_->recent()),
         "interaction.appearance_recent");
    return;
  case InteractionOperation::appearance_trigger: {
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    const auto found = std::ranges::find(request.interaction.command_options,
                                         "fixture", &InteractionOption::name);
    if (found == request.interaction.command_options.end() ||
        std::get_if<std::string>(&found->value) == nullptr ||
        *std::get_if<std::string>(&found->value) != "owner_live_safe")
      throw std::invalid_argument{"Owner live fixture is invalid."};
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_.now().time_since_epoch())
            .count();
    edit(request.interaction,
         text_message(
             appearances_->trigger_owner_live_safe(AppearanceSimulationRequest{
                 .fixture = "owner_live_safe",
                 .idempotency_key = "appearance.trigger:" +
                                    request.interaction.interaction_id.str(),
                 .correlation_id = request.interaction.correlation_id,
                 .owner_user_id = request.interaction.user_id,
                 .now_ms = timestamp,
                 .candidate_id = {},
                 .event_id = {}})),
         "interaction.appearance_trigger");
    return;
  }
  case InteractionOperation::appearance_disable:
  case InteractionOperation::appearance_enable:
    if (!appearances_)
      throw std::runtime_error{"Appearance service is unavailable."};
    edit(request.interaction,
         text_message(appearances_->set_global_disabled(
             request.interaction.user_id,
             request.operation == InteractionOperation::appearance_disable,
             "appearance.kill:" + request.interaction.interaction_id.str(),
             request.interaction.correlation_id)),
         "interaction.appearance_kill_switch");
    return;
  case InteractionOperation::tarot_balance:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->balance(request.interaction),
         "interaction.tarot_balance");
    return;
  case InteractionOperation::tarot_history:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->history(request.interaction),
         "interaction.tarot_history");
    return;
  case InteractionOperation::tarot_standings:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->standings(request.interaction),
         "interaction.tarot_standings");
    return;
  case InteractionOperation::tarot_standings_visibility:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction,
         tarot_->set_standings_visibility(request.interaction),
         "interaction.tarot_standings_visibility");
    return;
  case InteractionOperation::tarot_grace:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->start_grace(request.interaction),
         "interaction.tarot_grace");
    return;
  case InteractionOperation::tarot_trial:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->start_trial(request.interaction),
         "interaction.tarot_trial");
    return;
  case InteractionOperation::tarot_draw:
    if (!tarot_draws_)
      throw std::runtime_error{"Tarot draw service is unavailable."};
    edit(request.interaction, tarot_draws_->draw(request.interaction),
         "interaction.tarot_draw");
    return;
  case InteractionOperation::tarot_record:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->record(request.interaction),
         "interaction.tarot_record");
    return;
  case InteractionOperation::tarot_house_offers:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->offers(request.interaction),
         "interaction.tarot_house_offers");
    return;
  case InteractionOperation::tarot_house_play:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->play(request.interaction),
         "interaction.tarot_house_play");
    return;
  case InteractionOperation::tarot_house_history:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->history(request.interaction),
         "interaction.tarot_house_history");
    return;
  case InteractionOperation::tarot_component:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->apply_component(request.interaction),
         "interaction.tarot_component");
    return;
  case InteractionOperation::tarot_house_component:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction,
         tarot_house_->apply_component(request.interaction),
         "interaction.tarot_house_component");
    return;
  case InteractionOperation::tarot_adjust:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->adjust(request.interaction),
         "interaction.tarot_adjust");
    return;
  case InteractionOperation::tarot_reverse:
    if (!tarot_)
      throw std::runtime_error{"Tarot service is unavailable."};
    edit(request.interaction, tarot_->reverse(request.interaction),
         "interaction.tarot_reverse");
    return;
  case InteractionOperation::tarot_economy:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->economy(request.interaction),
         "interaction.tarot_economy");
    return;
  case InteractionOperation::tarot_draw_test:
    if (!tarot_draws_)
      throw std::runtime_error{"Tarot draw service is unavailable."};
    edit(request.interaction, tarot_draws_->draw(request.interaction, true),
         "interaction.tarot_draw_test");
    return;
  case InteractionOperation::tarot_draw_replay:
    if (!tarot_draws_)
      throw std::runtime_error{"Tarot draw service is unavailable."};
    edit(request.interaction, tarot_draws_->replay(request.interaction),
         "interaction.tarot_draw_replay");
    return;
  case InteractionOperation::tarot_house_offer:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->offer_test(request.interaction),
         "interaction.tarot_house_offer");
    return;
  case InteractionOperation::tarot_house_resolve:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->resolve(request.interaction),
         "interaction.tarot_house_resolve");
    return;
  case InteractionOperation::tarot_house_deadline:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->deadline(request.interaction),
         "interaction.tarot_house_deadline");
    return;
  case InteractionOperation::tarot_house_cleanup:
    if (!tarot_house_)
      throw std::runtime_error{"Tarot House service is unavailable."};
    edit(request.interaction, tarot_house_->cleanup_test(request.interaction),
         "interaction.tarot_house_cleanup");
    return;
  case InteractionOperation::tarot_integration_preview:
    if (!tarot_integration_)
      throw std::runtime_error{"Tarot integration service is unavailable."};
    edit(request.interaction, tarot_integration_->preview(request.interaction),
         "interaction.tarot_integration_preview");
    return;
  case InteractionOperation::tarot_integration_retry:
    if (!tarot_integration_)
      throw std::runtime_error{"Tarot integration service is unavailable."};
    edit(request.interaction, tarot_integration_->retry(request.interaction),
         "interaction.tarot_integration_retry");
    return;
  case InteractionOperation::wager_create:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->create(request.interaction),
         "interaction.wager_create");
    return;
  case InteractionOperation::wager_preview:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->preview(request.interaction),
         "interaction.wager_preview");
    return;
  case InteractionOperation::wager_component:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->apply_component(request.interaction),
         "interaction.wager_component");
    return;
  case InteractionOperation::wager_action:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->action(request.interaction),
         "interaction.wager_action");
    return;
  case InteractionOperation::wager_outcome:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->outcome(request.interaction),
         "interaction.wager_outcome");
    return;
  case InteractionOperation::wager_evidence:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->evidence(request.interaction),
         "interaction.wager_evidence");
    return;
  case InteractionOperation::wager_judgment:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->judgment(request.interaction),
         "interaction.wager_judgment");
    return;
  case InteractionOperation::wager_history:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->wagers(request.interaction),
         "interaction.wager_history");
    return;
  case InteractionOperation::wager_disputes:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->disputes(request.interaction),
         "interaction.wager_disputes");
    return;
  case InteractionOperation::wager_test_role:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->set_test_role(request.interaction),
         "interaction.wager_test_role");
    return;
  case InteractionOperation::wager_test_deadline:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->force_test_deadline(request.interaction),
         "interaction.wager_test_deadline");
    return;
  case InteractionOperation::wager_test_cleanup:
    if (!wagers_)
      throw std::runtime_error{"Wager service is unavailable."};
    edit(request.interaction, wagers_->cleanup_test_wager(request.interaction),
         "interaction.wager_test_cleanup");
    return;
  case InteractionOperation::vox_narration_preview:
  case InteractionOperation::vox_narration_enqueue: {
    if (!vox_narration_)
      throw std::runtime_error{"Vox narration service is unavailable."};
    const auto option =
        std::ranges::find(request.interaction.command_options, "reference",
                          &InteractionOption::name);
    if (option == request.interaction.command_options.end())
      throw std::invalid_argument{"A narration event reference is required."};
    const auto *reference = std::get_if<std::string>(&option->value);
    if (reference == nullptr)
      throw std::invalid_argument{"The narration event reference is invalid."};
    const VoxNarrationControlContext receipt_context{
        .idempotency_key =
            "vox:interaction:" + request.interaction.interaction_id.str(),
        .operation =
            request.operation == InteractionOperation::vox_narration_preview
                ? "narration_preview"
                : "narration_enqueue",
        .actor_user_id = request.interaction.user_id.str(),
        .guild_id = request.interaction.guild_id.str(),
        .channel_id = request.interaction.channel_id.str(),
        .request_fingerprint = *reference,
        .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock_.now().time_since_epoch())
                      .count()};
    const std::string_view narration_category =
        request.operation == InteractionOperation::vox_narration_preview
            ? "interaction.vox_narration_preview"
            : "interaction.vox_narration_enqueue";
    if (const auto receipt = vox_narration_->control_receipt(receipt_context)) {
      edit(request.interaction, text_message(*receipt), narration_category);
      return;
    }
    const auto complete = [&](std::string message,
                              const std::string_view category) {
      auto response = vox_narration_->record_control_receipt(
          receipt_context, std::move(message));
      edit(request.interaction, text_message(std::move(response)), category);
    };
    const auto candidate = vox_narration_->preview(*reference);
    if (!candidate) {
      complete("That event is unknown, stale, private, or ineligible for "
               "narration.",
               narration_category);
      return;
    }
    if (request.operation == InteractionOperation::vox_narration_enqueue) {
      if (!candidate->is_test) {
        complete("Narration enqueue accepts only explicitly tagged owner-test "
                 "events.",
                 "interaction.vox_narration_enqueue");
        return;
      }
      auto response =
          vox_narration_->enqueue_with_receipt(*reference, receipt_context);
      edit(request.interaction, text_message(std::move(response)),
           "interaction.vox_narration_enqueue");
      return;
    }
    std::ostringstream output;
    output << "Narration projection "
           << shortened_persistent_id(candidate->source_event_id) << ": "
           << candidate->event_type << ", feature "
           << vox_narration_feature_name(candidate->feature) << ", rank "
           << static_cast<unsigned int>(candidate->rank)
           << ". No line was generated and no speech budget was used.";
    complete(output.str(), "interaction.vox_narration_preview");
    return;
  }
  case InteractionOperation::vox_narration_recent: {
    if (!vox_narration_)
      throw std::runtime_error{"Vox narration service is unavailable."};
    const auto recent = vox_narration_->recent();
    std::ostringstream output;
    output << "Recent Vox narration audit (line content omitted):";
    if (recent.empty())
      output << " none.";
    for (const auto &item : recent) {
      output << "\n- " << shortened_persistent_id(item.intent_id) << " "
             << item.feature << "/" << item.state;
      if (item.reason)
        output << " (" << *item.reason << ")";
    }
    edit(request.interaction, text_message(output.str()),
         "interaction.vox_narration_recent");
    return;
  }
  case InteractionOperation::vox_transcript_propose:
    if (!voice_listening_)
      throw std::runtime_error{"Voice listening service is unavailable."};
    edit(request.interaction,
         voice_listening_->propose_transcript(request.interaction),
         "interaction.voice_transcript_propose");
    return;
  case InteractionOperation::vox_listen_start:
  case InteractionOperation::vox_listen_stop:
  case InteractionOperation::vox_listening_disable:
  case InteractionOperation::vox_listening_enable: {
    if (!voice_listening_) {
      edit(request.interaction,
           text_message("Voice listening is unavailable in this build."),
           "interaction.voice_listening_unavailable");
      return;
    }
    const auto interaction = request.interaction;
    const auto callback_fence = callbacks_;
    const auto editor = std::make_shared<
        interaction_handler_detail::SequencedInteractionEditor>(
        [this, interaction, callback_fence](InteractionMessage result,
                                            DeliveryCallback receipt) {
          const auto pending_receipt =
              std::make_shared<DeliveryNotifier>(std::move(receipt));
          try {
            const auto invoked = callback_fence->invoke(
                [this, interaction, result = std::move(result),
                 pending_receipt]() mutable {
                  edit(interaction, std::move(result),
                       "interaction.voice_listening",
                       [pending_receipt](const DeliveryResult delivery) {
                         pending_receipt->notify(delivery);
                       });
                });
            if (!invoked)
              pending_receipt->notify(DeliveryResult::permanent_failure);
          } catch (...) {
            pending_receipt->notify(DeliveryResult::unknown_outcome);
          }
        });
    const auto completion_started = std::make_shared<std::atomic_bool>();
    auto confirmed_completion = [editor,
                                 completion_started](InteractionMessage result,
                                                     DeliveryCallback receipt) {
      editor->submit(std::move(result), std::move(receipt),
                     completion_started->exchange(true));
    };
    SubmitResult submitted{SubmitResult::stopping};
    if (request.operation == InteractionOperation::vox_listen_start) {
      const auto option =
          std::ranges::find(request.interaction.command_options, "duration",
                            &InteractionOption::name);
      const auto *duration = option == request.interaction.command_options.end()
                                 ? nullptr
                                 : std::get_if<std::string>(&option->value);
      if (duration == nullptr ||
          (*duration != "5" && *duration != "10" && *duration != "15"))
        throw std::invalid_argument{
            "A 5, 10, or 15 second duration is required."};
      submitted = voice_listening_->listen_start(
          request.interaction, static_cast<std::size_t>(std::stoul(*duration)),
          std::move(confirmed_completion));
    } else {
      auto completion = [confirmed_completion =
                             std::move(confirmed_completion)](
                            InteractionMessage result) mutable {
        confirmed_completion(std::move(result), {});
      };
      if (request.operation == InteractionOperation::vox_listen_stop) {
        const auto window = parse_voice_control(request.interaction.custom_id,
                                                voice_listening_stop_prefix);
        submitted = voice_listening_->listen_stop(
            request.interaction, std::move(completion), window);
      } else {
        submitted = voice_listening_->set_kill_switch(
            request.interaction,
            request.operation == InteractionOperation::vox_listening_disable,
            std::move(completion), request.voice_disable_generation);
      }
    }
    if (submitted != SubmitResult::accepted) {
      edit(request.interaction,
           text_message("Voice listening is busy or shutting down. Try again."),
           "interaction.voice_listening_queue");
    }
    return;
  }
  case InteractionOperation::vox_summon:
  case InteractionOperation::vox_status:
  case InteractionOperation::vox_leave:
  case InteractionOperation::vox_say:
  case InteractionOperation::vox_mute:
  case InteractionOperation::vox_voice:
  case InteractionOperation::vox_test_disconnect:
  case InteractionOperation::vox_speech_test: {
    if (!vox_)
      throw std::runtime_error{"Vox service is unavailable."};
    const auto interaction = request.interaction;
    const auto callback_fence = callbacks_;
    auto completion = [this, interaction,
                       callback_fence](VoxCommandResult result) mutable {
      try {
        static_cast<void>(
            callback_fence->invoke([this, interaction = std::move(interaction),
                                    result = std::move(result)]() mutable {
              edit(interaction, text_message(result.message),
                   "interaction.vox");
            }));
      } catch (...) {
      }
    };
    VoxCommandContext context{
        .guild_id = request.interaction.guild_id,
        .text_channel_id = request.interaction.channel_id,
        .actor_user_id = request.interaction.user_id,
        .owner_user_id = {},
        .interaction_idempotency_key =
            "vox:interaction:" + request.interaction.interaction_id.str(),
        .correlation_id = request.interaction.correlation_id,
        .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock_.now().time_since_epoch())
                      .count(),
    };
    const auto string_option =
        [&request](const std::string_view name,
                   const bool required) -> std::optional<std::string> {
      const auto found = std::ranges::find(request.interaction.command_options,
                                           name, &InteractionOption::name);
      if (found == request.interaction.command_options.end()) {
        if (required)
          throw std::invalid_argument{"A Vox command option is required."};
        return std::nullopt;
      }
      const auto *value = std::get_if<std::string>(&found->value);
      if (value == nullptr)
        throw std::invalid_argument{"A Vox command option is invalid."};
      return *value;
    };
    std::optional<std::string> argument;
    if (request.operation == InteractionOperation::vox_say)
      argument = string_option("text", true);
    else if (request.operation == InteractionOperation::vox_mute)
      argument = string_option("duration", true);
    else if (request.operation == InteractionOperation::vox_voice)
      argument = string_option("voice", false);
    else if (request.operation == InteractionOperation::vox_speech_test)
      argument = string_option("scenario", true);
    const auto submitted =
        request.operation == InteractionOperation::vox_summon
            ? vox_->summon(std::move(context), std::move(completion))
        : request.operation == InteractionOperation::vox_status
            ? vox_->status(std::move(context), std::move(completion))
        : request.operation == InteractionOperation::vox_leave
            ? vox_->leave(std::move(context), std::move(completion))
        : request.operation == InteractionOperation::vox_say
            ? vox_->say(std::move(context), std::move(*argument),
                        std::move(completion))
        : request.operation == InteractionOperation::vox_mute
            ? vox_->mute(std::move(context), std::move(*argument),
                         std::move(completion))
        : request.operation == InteractionOperation::vox_voice
            ? vox_->voice(std::move(context), std::move(argument),
                          std::move(completion))
        : request.operation == InteractionOperation::vox_speech_test
            ? vox_->speech_test(std::move(context), std::move(*argument),
                                std::move(completion))
            : vox_->test_disconnect(std::move(context), std::move(completion));
    if (submitted != SubmitResult::accepted) {
      edit(request.interaction,
           text_message("Vox is handling another request. Please try again."),
           "interaction.vox_queue");
    }
    return;
  }
  }
}

void InteractionHandler::ensure_user(const IncomingInteraction &interaction) {
  const auto observed_at_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          clock_.now().time_since_epoch())
          .count();
  identities_.ensure_user(DiscordUserRecord{
      .user_id = interaction.user_id,
      .display_name = cache_value(interaction.display_name),
      .username = cache_value(interaction.username),
      .is_bot = false,
      .observed_at_ms = observed_at_ms,
  });
  for (const auto &resolved : interaction.resolved_users) {
    identities_.ensure_user(DiscordUserRecord{
        .user_id = resolved.user_id,
        .display_name = cache_value(resolved.display_name),
        .username = cache_value(resolved.username),
        .is_bot = resolved.is_bot,
        .observed_at_ms = observed_at_ms,
    });
  }
}

void InteractionHandler::edit(
    const IncomingInteraction &interaction, InteractionMessage message,
    const std::string_view diagnostic_category,
    DeliveryCallback delivery_callback) const noexcept {
  std::shared_ptr<DeliveryNotifier> pending_delivery;
  try {
    pending_delivery =
        std::make_shared<DeliveryNotifier>(std::move(delivery_callback));
    if (!interaction.responder) {
      throw std::runtime_error{"Interaction responder is missing."};
    }
    const auto correlation_id = interaction.correlation_id;
    const auto category = std::string{diagnostic_category};
    const auto callbacks = callbacks_;
    interaction.responder->edit_original(
        message, [this, callbacks, correlation_id, category,
                  pending_delivery](const DeliveryResult result) {
          pending_delivery->notify(result);
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
    if (pending_delivery)
      pending_delivery->notify(DeliveryResult::permanent_failure);
    else if (delivery_callback) {
      try {
        delivery_callback(DeliveryResult::permanent_failure);
      } catch (...) {
      }
    }
    diagnostics_.emit({DiagnosticSeverity::error,
                       std::string{diagnostic_category}, error.what(),
                       interaction.correlation_id});
  } catch (...) {
    if (pending_delivery)
      pending_delivery->notify(DeliveryResult::unknown_outcome);
    else if (delivery_callback) {
      try {
        delivery_callback(DeliveryResult::unknown_outcome);
      } catch (...) {
      }
    }
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
