#include <dpp/dpp.h>
#include <helpCommand.hpp>

bool helpCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!help")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void helpCommand::executeCommand(const dpp::message_create_t& event)
{

  dpp::message response_message;
  std::string response_content;
  response_content = "I support the following commands:\n"
  response_content += "    !date - Print the current date\n";
  response_content += "    !roll - Roll up to a given number. Defaults to 100.\n";
  response_content += "    !join - Command me to join the voice channel you're currently in\n";
  response_content += "    !repo - Link the GitHub public repo for this bot\n";
  response_content += "    !vote - Currently disabled due to change in command type";

  response_message.content=response_content;
  event.reply(response_message);
}
