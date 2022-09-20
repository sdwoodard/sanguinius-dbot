#include <dpp/dpp.h>
#include <diceCommand.hpp>

#include <vector>
#include <sstream>
#include <string>

void diceCommand::commandCallBack(std::string command, const dpp::message_create_t& event)
{

  if (command == "!dice")
  {
    this->executeCommand(event);
  }

}

void diceCommand::executeCommand(const dpp::message_create_t& event)
{
  std::srand(time(0));
  int botDiceRoll = rand();
  std::stringstream ss;
  ss << "I rolled " << botDiceRoll;
  std::string s = ss.str();
  dpp::message response_message;
  response_message.content = s;


  event.reply(response_message);
}
