#include <dpp/dpp.h>
#include <dateCommand.hpp>

void dateCommand::commandCallBack(std::string command, const dpp::message_create_t& event)
{

  if (command == "!date")
  {
    this->executeCommand(event);
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
