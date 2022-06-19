#include <dpp/dpp.h>
#include <dateCommand.hpp>

void dateCommand::registerCommand(std::shared_ptr<dpp::cluster> bot)
{
  dpp::slashcommand datecommand("date", "Return the current date", bot->me.id);
  bot->global_command_create(datecommand);
}

void dateCommand::commandCallBack(std::string command, const dpp::slashcommand_t& event)
{

  if (command == "date")
  {
    this->executeCommand(event);
  }

}

void dateCommand::executeCommand(const dpp::slashcommand_t& event)
{

  dpp::message response_message;
  time_t now = time(0);
  char* dt = ctime(&now);
  std::ostringstream oss;
  oss << "It is now " << dt;
  response_message.content = oss.str();
  event.reply(response_message);
}
