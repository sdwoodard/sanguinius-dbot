#include <dpp/dpp.h>
#include <voteCommand.hpp>

bool voteCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!vote")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void voteCommand::executeCommand(const dpp::message_create_t& event)
{

  dpp::message response_message;
  response_message.content = "This command is undergoing maintenance.";
  event.reply(response_message);

}
