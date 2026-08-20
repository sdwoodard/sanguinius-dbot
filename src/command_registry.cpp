#include "sanguinius/command_registry.hpp"
#include "sanguinius/chronicle.hpp"

#include <sstream>
#include <utility>

namespace sanguinius {

CommandCatalog command_catalog(const bool admin_commands_enabled,
                               const bool chronicle_enabled,
                               const bool tarot_enabled) {
  CommandCatalog catalog{
      .version = command_catalog_version,
      .commands =
          {
              CommandDefinition{
                  .name = "sanguinius",
                  .description = "Consult Sanguinius.",
                  .subcommands =
                      {
                          {"status", "Show a private status summary."},
                          {"inbox", "Open the next sealed notice privately."},
                          {"privacy",
                           "Review private-data and voice settings."},
                          {"appearance-callbacks",
                           "Privately enable or disable appearance callbacks.",
                           {{CommandOptionKind::string,
                             "mode",
                             "Appearance callback preference.",
                             true,
                             2,
                             3,
                             {{"On", "on"}, {"Off", "off"}}}}},
                          {"appearance-feedback",
                           "Privately respond to a delivered appearance.",
                           {{CommandOptionKind::string,
                             "response",
                             "Your private response.",
                             true,
                             4,
                             12,
                             {{"More like this", "more"},
                              {"Less like this", "less"},
                              {"Not relevant", "not_relevant"}}},
                            {CommandOptionKind::string, "reference",
                             "Optional decision or message reference.", false,
                             4, 36}}},
                      },
                  .subcommand_groups = {CommandSubcommandGroupDefinition{
                      .name = "quiet",
                      .description = "Set server-wide appearance quiet.",
                      .subcommands =
                          {{"for",
                            "Quiet appearances for a duration.",
                            {{CommandOptionKind::string,
                              "duration",
                              "Quiet duration.",
                              true,
                              2,
                              2,
                              {{"2 hours", "2h"}}}}},
                           {"tonight", "Quiet until tomorrow at 10:00 AM."},
                           {"until",
                            "Quiet until the next local time.",
                            {{CommandOptionKind::string, "time",
                              "Local time as HH:MM.", true, 5, 5}}},
                           {"off", "End quiet early if authorized."}}}},
              },
          },
  };
  if (chronicle_enabled) {
    catalog.commands.push_back(CommandDefinition{
        .name = "chronicle",
        .description = "Consult or amend the Living Chronicle.",
        .subcommands =
            {
                CommandSubcommandDefinition{.name = "remember",
                                            .description =
                                                "Explicitly remember something "
                                                "after a private preview."},
                CommandSubcommandDefinition{
                    .name = "recall",
                    .description =
                        "Privately recall up to five visible records.",
                    .options =
                        {
                            {CommandOptionKind::string, "query",
                             "Optional literal text to find.", false, 1, 200},
                            {CommandOptionKind::user, "participant",
                             "Optional named participant.", false},
                            {CommandOptionKind::string,
                             "type",
                             "Optional Chronicle entry type.",
                             false,
                             4,
                             20,
                             {{"Quote", "quote"},
                              {"Deed", "deed"},
                              {"Prediction", "prediction"},
                              {"Incident", "incident"},
                              {"Custom", "custom"},
                              {"Session summary", "session_summary"},
                              {"Title award", "title_award"}}},
                            {CommandOptionKind::string, "from",
                             "Start date, YYYY-MM-DD.", false, 10, 10},
                            {CommandOptionKind::string, "to",
                             "End date, YYYY-MM-DD.", false, 10, 10},
                        }},
                CommandSubcommandDefinition{
                    .name = "timeline",
                    .description = "Show recent shared canon entries.",
                    .options = {CommandOptionDefinition{
                        .kind = CommandOptionKind::string,
                        .name = "period",
                        .description = "Time period to display.",
                        .required = false,
                        .minimum_length = 2,
                        .maximum_length = 3,
                        .choices = {{"Last 7 days", "7d"},
                                    {"Last 30 days", "30d"},
                                    {"All time", "all"}}}}},
                CommandSubcommandDefinition{
                    .name = "forget",
                    .description =
                        "Retract by reference or privately choose a record.",
                    .options = {CommandOptionDefinition{
                        .kind = CommandOptionKind::string,
                        .name = "reference",
                        .description = "Optional record reference prefix.",
                        .required = false,
                        .minimum_length = 4,
                        .maximum_length = 36}}},
                CommandSubcommandDefinition{
                    .name = "profile",
                    .description =
                        "Show a private self or public-safe profile.",
                    .options = {CommandOptionDefinition{
                        .kind = CommandOptionKind::user,
                        .name = "user",
                        .description =
                            "Optional member for a public-safe profile.",
                        .required = false}}},
                CommandSubcommandDefinition{
                    .name = "callbacks",
                    .description =
                        "Privately enable or disable memory callbacks.",
                    .options = {CommandOptionDefinition{
                        .kind = CommandOptionKind::string,
                        .name = "mode",
                        .description = "Memory callback preference.",
                        .required = true,
                        .minimum_length = 2,
                        .maximum_length = 3,
                        .choices = {{"On", "on"}, {"Off", "off"}}}}},
            },
        .subcommand_groups =
            {
                CommandSubcommandGroupDefinition{
                    .name = "session",
                    .description = "Manage Chronicle sessions.",
                    .subcommands =
                        {
                            {"start", "Open a Chronicle session."},
                            {"status", "Show the active or latest session."},
                            {"close", "Close the active Chronicle session."},
                            {"edit",
                             "Edit a pending chapter draft.",
                             {{CommandOptionKind::string, "reference",
                               "Full draft reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected draft revision.", true, 1, 20},
                              {CommandOptionKind::string, "title",
                               "Chapter title.", true, 1, 100},
                              {CommandOptionKind::string, "summary",
                               "Chapter summary.", true, 1, 1000}}},
                            {"approve",
                             "Approve a pending chapter draft.",
                             {{CommandOptionKind::string, "reference",
                               "Full draft reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected draft revision.", true, 1, 20}}},
                            {"reject",
                             "Reject a pending chapter draft.",
                             {{CommandOptionKind::string, "reference",
                               "Full draft reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected draft revision.", true, 1, 20}}},
                        }},
                CommandSubcommandGroupDefinition{
                    .name = "title",
                    .description = "Manage Chronicle titles.",
                    .subcommands =
                        {
                            {"propose",
                             "Propose an owner-curated title.",
                             {{CommandOptionKind::user, "recipient",
                               "Title recipient.", true},
                              {CommandOptionKind::string, "title",
                               "Title wording.", true, 1, 100},
                              {CommandOptionKind::string, "description",
                               "Why it was earned.", true, 1, 500}}},
                            {"list",
                             "List visible title grants.",
                             {{CommandOptionKind::user, "recipient",
                               "Optional title recipient.", false},
                              {CommandOptionKind::string, "page",
                               "Optional result page.", false, 1, 3}}},
                            {"approve",
                             "Activate a proposed title.",
                             {{CommandOptionKind::string, "reference",
                               "Full grant reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected grant revision.", true, 1, 20}}},
                            {"reject",
                             "Reject a proposed title.",
                             {{CommandOptionKind::string, "reference",
                               "Full grant reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected grant revision.", true, 1, 20}}},
                            {"feature",
                             "Choose an active featured title.",
                             {{CommandOptionKind::string, "reference",
                               "Full grant reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected grant revision.", true, 1, 20}}},
                            {"revoke",
                             "Revoke an active title.",
                             {{CommandOptionKind::string, "reference",
                               "Full grant reference.", true, 36, 36},
                              {CommandOptionKind::string, "revision",
                               "Expected grant revision.", true, 1, 20}}},
                        }},
                CommandSubcommandGroupDefinition{
                    .name = "anniversaries",
                    .description = "Manage Chronicle anniversaries.",
                    .subcommands =
                        {{"on", "Enable Chronicle anniversary reminders."},
                         {"off", "Disable Chronicle anniversary reminders."}}},
            },
    });
    catalog.commands.push_back(CommandDefinition{
        .name = "Canonize in the Chronicle",
        .description = {},
        .subcommands = {},
        .kind = ApplicationCommandKind::message_context,
    });
  }
  if (tarot_enabled) {
    const std::vector<CommandOptionChoiceDefinition> standings_choices{
        {"Public", "public"}, {"Private", "private"}};
    const std::vector<CommandOptionChoiceDefinition>
        recovery_visibility_choices{{"Public flavor", "public"},
                                    {"Private", "private"}};
    catalog.commands.push_back(CommandDefinition{
        .name = "tarot",
        .description = "Consult the Emperor's Tarot Fate ledger.",
        .subcommands =
            {
                {"balance", "Show your private Fate balance."},
                {"history", "Show your private immutable Fate history."},
                {"standings", "Show public opted-in Fate standings."},
                {"standings-visibility",
                 "Choose whether you appear in public standings.",
                 {{CommandOptionKind::string, "mode",
                   "Public standings preference.", true, 6, 7,
                   standings_choices}}},
                {"grace",
                 "Seek Grace of the Throne when Fate is nearly gone.",
                 {{CommandOptionKind::string, "visibility",
                   "Visibility of the neutral completion flavor.", false, 6, 7,
                   recovery_visibility_choices}}},
                {"trial",
                 "Take a Trial of Renewal when Fate is low.",
                 {{CommandOptionKind::string, "visibility",
                   "Visibility of the neutral completion flavor.", false, 6, 7,
                   recovery_visibility_choices}}},
            },
    });
  }
  CommandDefinition owner_controls{
      .name = "sang-admin",
      .description = "Owner-only Sanguinius controls.",
      .subcommands = {},
      .subcommand_groups =
          {
              CommandSubcommandGroupDefinition{
                  .name = "appearance",
                  .description = "Inspect and control appearances.",
                  .subcommands =
                      {
                          {"disable", "Persistently disable live appearances."},
                          {"enable",
                           "Clear the global appearance kill switch."},
                      }},
          },
  };
  if (admin_commands_enabled) {
    owner_controls.subcommands = {
        {"health", "Show the private redacted health snapshot."},
        {"work-recent", "Inspect recent redacted durable work."},
        {"work-dead", "Inspect failed and dead durable work."},
        {"test-notice", "Create a private self-targeted test notice."},
        {"test-schedule-notice",
         "Schedule a private self-targeted test notice."},
        {"test-public-retry", "Exercise one synthetic public delivery retry."},
    };
    owner_controls.subcommand_groups.front().subcommands = {
        {"simulate",
         "Create an auditable dry-run fixture.",
         {{CommandOptionKind::string,
           "fixture",
           "Sanitized fixture to evaluate.",
           true,
           8,
           64,
           {{"Lively game-night banter", "lively_game_night_banter"},
            {"One-person quiet channel", "one_person_quiet_channel"},
            {"Bot just spoke", "bot_just_spoke"},
            {"Quiet hours", "quiet_hours"},
            {"Manual quiet", "manual_quiet"},
            {"Sensitive serious conversation",
             "sensitive_serious_conversation"},
            {"Christianity", "christianity"},
            {"Chronicle anniversary", "chronicle_anniversary"},
            {"Repeated inside joke", "repeated_inside_joke_on_cooldown"},
            {"Tarot settlement", "tarot_settlement"},
            {"Opted-out participant", "opted_out_participant"},
            {"Stale candidate", "stale_candidate"},
            {"Owner dry run", "owner_dry_run"}}}}},
        {"preview",
         "Inspect one stored redacted decision.",
         {{CommandOptionKind::string, "reference",
           "Full candidate or decision UUID.", true, 36, 36}}},
        {"recent", "Inspect ten recent redacted decisions."},
        {"trigger",
         "Queue one auditable owner live test.",
         {{CommandOptionKind::string,
           "fixture",
           "Curated live fixture.",
           true,
           8,
           64,
           {{"Owner live safe", "owner_live_safe"}}}}},
        {"disable", "Persistently disable live appearances."},
        {"enable", "Clear the global appearance kill switch."},
    };
    if (chronicle_enabled) {
      owner_controls.subcommands.push_back(
          {"test-anniversary", "Exercise an exactly-once test anniversary."});
    }
    if (tarot_enabled) {
      owner_controls.subcommand_groups.push_back(
          CommandSubcommandGroupDefinition{
              .name = "tarot",
              .description = "Owner-only self-targeted Tarot test controls.",
              .subcommands = {
                  CommandSubcommandDefinition{
                      .name = "adjust",
                      .description = "Apply a balanced [TEST] adjustment.",
                      .options =
                          {CommandOptionDefinition{
                               .kind = CommandOptionKind::integer,
                               .name = "amount",
                               .description = "Nonzero signed Fate delta.",
                               .required = true,
                               .minimum_integer = minimum_tarot_adjustment,
                               .maximum_integer = maximum_tarot_adjustment},
                           CommandOptionDefinition{
                               .kind = CommandOptionKind::string,
                               .name = "reason",
                               .description = "Audited test reason.",
                               .required = true,
                               .minimum_length = 1,
                               .maximum_length = 200}}},
                  CommandSubcommandDefinition{
                      .name = "reverse",
                      .description =
                          "Exactly reverse an eligible [TEST] transaction.",
                      .options = {CommandOptionDefinition{
                                      .kind = CommandOptionKind::string,
                                      .name = "transaction",
                                      .description = "Full transaction UUID.",
                                      .required = true,
                                      .minimum_length = 36,
                                      .maximum_length = 36},
                                  CommandOptionDefinition{
                                      .kind = CommandOptionKind::string,
                                      .name = "reason",
                                      .description = "Audited reversal reason.",
                                      .required = true,
                                      .minimum_length = 1,
                                      .maximum_length = 200}}},
              }});
    }
  }
  catalog.commands.push_back(std::move(owner_controls));
  return catalog;
}

std::string canonical_command_snapshot(const CommandCatalog &catalog) {
  std::ostringstream output;
  output << "catalog_version=" << catalog.version << '\n';
  for (const auto &command : catalog.commands) {
    output << "command=" << static_cast<int>(command.kind) << '|'
           << command.name << '|' << command.description << '\n';
    for (const auto &subcommand : command.subcommands) {
      output << "subcommand=" << subcommand.name << '|'
             << subcommand.description << '\n';
      for (const auto &option : subcommand.options) {
        output << "option=" << static_cast<int>(option.kind) << '|'
               << option.name << '|' << option.description << '|'
               << option.required << '|' << option.minimum_length << '|'
               << option.maximum_length << '|'
               << (option.minimum_integer
                       ? std::to_string(*option.minimum_integer)
                       : "none")
               << '|'
               << (option.maximum_integer
                       ? std::to_string(*option.maximum_integer)
                       : "none")
               << '\n';
        for (const auto &choice : option.choices) {
          output << "choice=" << choice.name << '|' << choice.value << '\n';
        }
      }
    }
    for (const auto &group : command.subcommand_groups) {
      output << "group=" << group.name << '|' << group.description << '\n';
      for (const auto &subcommand : group.subcommands) {
        output << "group_subcommand=" << subcommand.name << '|'
               << subcommand.description << '\n';
        for (const auto &option : subcommand.options) {
          output << "option=" << static_cast<int>(option.kind) << '|'
                 << option.name << '|' << option.description << '|'
                 << option.required << '|' << option.minimum_length << '|'
                 << option.maximum_length << '|'
                 << (option.minimum_integer
                         ? std::to_string(*option.minimum_integer)
                         : "none")
                 << '|'
                 << (option.maximum_integer
                         ? std::to_string(*option.maximum_integer)
                         : "none")
                 << '\n';
          for (const auto &choice : option.choices)
            output << "choice=" << choice.name << '|' << choice.value << '\n';
        }
      }
    }
  }
  return output.str();
}

bool CommandRegistrationCoordinator::begin() {
  const std::scoped_lock lock{mutex_};
  if (in_flight_) {
    return false;
  }
  in_flight_ = true;
  state_ = CommandRegistrationState::synchronizing;
  return true;
}

CommandCatalogFetchAction
CommandRegistrationCoordinator::catalog_fetched(const bool success,
                                                const bool matches) {
  const std::scoped_lock lock{mutex_};
  if (!in_flight_) {
    return CommandCatalogFetchAction::none;
  }
  if (!success) {
    in_flight_ = false;
    state_ = CommandRegistrationState::failed;
    return CommandCatalogFetchAction::none;
  }
  if (matches) {
    in_flight_ = false;
    state_ = CommandRegistrationState::synchronized;
    return CommandCatalogFetchAction::none;
  }
  return CommandCatalogFetchAction::update_required;
}

void CommandRegistrationCoordinator::catalog_updated(const bool success) {
  const std::scoped_lock lock{mutex_};
  if (!in_flight_) {
    return;
  }
  in_flight_ = false;
  state_ = success ? CommandRegistrationState::synchronized
                   : CommandRegistrationState::failed;
}

void CommandRegistrationCoordinator::cancel() noexcept {
  try {
    const std::scoped_lock lock{mutex_};
    in_flight_ = false;
  } catch (...) {
  }
}

CommandRegistrationState
CommandRegistrationCoordinator::state() const noexcept {
  try {
    const std::scoped_lock lock{mutex_};
    return state_;
  } catch (...) {
    return CommandRegistrationState::failed;
  }
}

} // namespace sanguinius
