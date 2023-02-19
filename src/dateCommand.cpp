#include <dpp/dpp.h>
#include <dateCommand.hpp>

bool dateCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!date")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void dateCommand::executeCommand(const dpp::message_create_t& event)
{

  dpp::message response_message;
  time_t now = time(0);
  char* dt = ctime(&now);
  std::ostringstream oss;
  oss << "It is now " << dt;
  response_message.content = oss.str();
  event.reply(response_message);
}
