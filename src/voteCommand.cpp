#include <dpp/dpp.h>
#include <voteCommand.hpp>

void voteCommand::registerCommand(std::shared_ptr<dpp::cluster> bot)
{
  dpp::slashcommand votecommand("vote", "Vote for your desired user", bot->me.id);
  votecommand.add_option(
    dpp::command_option(dpp::co_string, "user_vote", "The user's real name", true).
    add_choice(dpp::command_option_choice("Lucas", std::string("user_Lucas"))).
    add_choice(dpp::command_option_choice("Nicholas", std::string("user_Nicholas"))).
    add_choice(dpp::command_option_choice("Stephen", std::string("user_Stephen")))
  );

  bot->global_command_create(votecommand);
}

void voteCommand::commandCallBack(std::string command, const dpp::message_create_t& event)
{

  if (command == "vote")
  {
    this->executeCommand(event);
  }

}

void voteCommand::executeCommand(const dpp::message_create_t& event)
{

  dpp::message response_message;
  response_message.content = "This command is undergoing maintenance.";
  event.reply(response_message);

}
