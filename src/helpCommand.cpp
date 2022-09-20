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
  std::string response_content;
  response_content += "    !date - Print the current date\n";
  response_content += "    !dice - A dice roll made by Nicholas\n";
  response_content += "    !join - Command me to join the voice channel you're currently in\n";
  response_content += "    !repo - Link the GitHub public repo for this bot\n";
  response_content += "    !vote - Currently disabled due to change in command type";

  response_message.content=response_content;
  event.reply(response_message);
}
