#include <dpp/dpp.h>
#include <diceCommand.hpp>

#include <vector>
#include <sstream>
#include <string>

void diceCommand::commandCallBack(std::string command, const dpp::message_create_t& event)
{

  std::string keyword;
  std::size_t spaceLoc = command.find(' ');
  if (spaceLoc == std::string::npos)
  {
    keyword = command;
  }
  else
  {
    keyword = command.substr(0, spaceLoc);
  }


  std::cout << "Command is: " << command << std::endl;
  std::cout << "Keyword is: " << keyword << std::endl;

  if (keyword == "!dice")
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

  std::cout << "Ceiling is: " << ceilingLng << std::endl;

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
