#include <dpp/dpp.h>
#include <helpCommand.hpp>

void helpCommand::registerCommand(std::shared_ptr<dpp::cluster> bot)
{
  dpp::slashcommand helpcommand("help", "Provide usage", bot->me.id);
  bot->global_command_create(helpcommand);
}

void helpCommand::commandCallBack(std::string command, const dpp::slashcommand_t& event)
{

  if (command == "help")
  {
    this->executeCommand(event);
  }

}

void helpCommand::executeCommand(const dpp::slashcommand_t& event)
{

  dpp::message response_message;

  if (event.command.get_command_name() == "help")
  {
    response_message.content="Help yourself, human.";
  }

  event.reply(response_message);
}
