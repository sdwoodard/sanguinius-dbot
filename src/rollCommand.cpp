#include <dpp/dpp.h>
#include <rollCommand.hpp>

#include <vector>
#include <sstream>
#include <string>

bool rollCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!roll")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void rollCommand::executeCommand(const dpp::message_create_t& event)
{
  // Default value of the ceiling
  unsigned long ceilingLong = 100;

  // Take the second keyword from the given string (if it exists)
  std::istringstream iss(event.msg.content);
  std::string ceilingString;
  iss >> ceilingString >> ceilingString;

  // Check if the provided argument is valid.  If it is, overwrite the default ceiling
  if (!ceilingString.empty())
  {
    if (std::all_of(ceilingString.begin(), ceilingString.end(), ::isdigit))
    {
      unsigned long tmpCeilingLong = std::stoul(ceilingString);
      if (tmpCeilingLong > 0 && tmpCeilingLong <= std::numeric_limits<unsigned long>::max())
      {
        ceilingLong = tmpCeilingLong;
      }
    }
  }

  // Random number
  std::srand(time(0));
  int botRoll = rand() % ceilingLong;

  // Construct string
  std::stringstream ss;
  ss << event.msg.author.username << " has rolled " << botRoll << "/" << ceilingLong;
  std::string s = ss.str();
  dpp::message response_message;
  response_message.content = s;

  event.reply(response_message);
}
