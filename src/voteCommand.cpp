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

void voteCommand::commandCallBack(std::string command, const dpp::slashcommand_t& event)
{

  if (command == "vote")
  {
    this->executeCommand(event);
  }

}

void voteCommand::executeCommand(const dpp::slashcommand_t& event)
{

  dpp::message response_message;
  std::string vote_response = std::get<std::string>(event.get_parameter("user_vote"));
  if (vote_response == "user_Lucas")
  {
    response_message.content = "Registered vote for Mr. New Car.";
  }
  else if (vote_response == "user_Nicholas")
  {
    response_message.content = "Posting " + vote_response + "'s private information now.";
  }
  else if (vote_response == "user_Stephen")
  {
    response_message.content = vote_response + " is immune from this dark vote.";
  }
  else
  {
    response_message.content = "ERROR: Please only vote for Lucas or Nicholas.";
  }

  event.reply(response_message);
}
