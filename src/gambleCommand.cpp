#include <dpp/dpp.h>
#include <gambleCommand.hpp>

gambleCommand::gambleCommand(dpp::cluster* acBot, pointHandler* acPointHandler, 
  boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger)
:
  bot(acBot),
  mpcPointHandler(acPointHandler),
  mpcLogger(apcLogger)
{

}

bool gambleCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!gamble")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void gambleCommand::executeCommand(const dpp::message_create_t& event)
{
  std::string keyword, arg1, arg2;
  std::stringstream ss(event.msg.content);
  ss >> keyword >> arg1;
  getline(ss, arg2);

  std::stringstream lcResponse;
  if (arg1 == "start")
  {
    if (!ongoingGamble)
    {
      gambleMap.clear();
      ongoingGamble = true;
      lcResponse << event.msg.author.username << " has posed the question. Stake your wagers!";
    }
    else
    {
      lcResponse << "There is already a gamble in progress. Please wait until it is finished.";
    }
  }

  else if (arg1 == "wager")
  {
    if (ongoingGamble)
    {
      if (mpcPointHandler->getPoints(event.msg.author.id) >= std::stoi(arg2))
      {
        gambleMap.insert(std::pair<bool, dpp::snowflake>(true, event.msg.author.id));
        mpcPointHandler->updatePoints(event.msg.author.id, -std::stoi(arg2));
        gambleAmount += std::stoi(arg2);
        lcResponse << "I've accepted " << event.msg.author.username << "'s wager of " << arg2 << " points.";
      }
      else
      {
        lcResponse << event.msg.author.username << " does not have enough points to wager that amount.";
      }
    }
  }

  else if (arg1 == "results")
  {
    if (ongoingGamble)
    {
      if (gambleMap.size() > 1)
      {
        int lcRandom = rand() % gambleMap.size();
        std::map<bool, dpp::snowflake>::iterator lcIterator = gambleMap.begin();
        std::advance(lcIterator, lcRandom);
        mpcPointHandler->updatePoints(lcIterator->second, gambleAmount);
        lcResponse << "Congratulations to the winner for earning " << gambleAmount << " points!";
        gambleMap.clear();
        gambleAmount = 0;
        ongoingGamble = false;
      }
      else
      {
        lcResponse << "There are not enough people to gamble.";
      }
    }
  }

  else if (arg1 == "cancel")
  {
    if (ongoingGamble)
    {
      gambleMap.clear();
      gambleAmount = 0;
      ongoingGamble = false;
      lcResponse << "The gamble has been cancelled.";
    }
    else
    {
      lcResponse << "There is no gamble in progress.";
    }
  }

  else if (arg1 == "repeat")
  {
    if (ongoingGamble)
    {
      lcResponse << "The current question is: " << arg2;
    }
    else
    {
      lcResponse << "There is no gamble in progress.";
    }
  }

  else
  {
    lcResponse << "Invalid command. Please use !gamble start, !gamble wager, !gamble results, !gamble repeat or !gamble cancel.";
  }

}
