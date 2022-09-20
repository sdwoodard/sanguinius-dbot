#include <dpp/dpp.h>
#include <voteCommand.hpp>

void voteCommand::commandCallBack(std::string command, const dpp::message_create_t& event)
{

  if (command == "!vote")
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
