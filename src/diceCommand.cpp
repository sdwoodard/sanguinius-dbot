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
  // Random number ceiling
  std::istringstream iss(event.msg.content);
  std::string ceilingString;
  iss >> ceilingString >> ceilingString;
  long ceilingLong = std::stol(ceilingString);

  // Random number
  std::srand(time(0));
  int botDiceRoll = rand() % ceilingLong;

  // Construct string
  std::stringstream ss;
  ss << event.msg.author.username << " has rolled " << botDiceRoll;
  std::string s = ss.str();
  dpp::message response_message;
  response_message.content = s;


  event.reply(response_message);
}
