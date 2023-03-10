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
  std::string arg1, arg2;
  std::stringstream ss(event.msg.content);
  ss.ignore(std::numeric_limits<std::streamsize>::max(), ' '); // ignore the first word
  ss >> arg1;
  getline(ss, arg2);

  std::stringstream lcResponse;
  if (arg1 == "start")
  {
    if (!ongoingGamble)
    {
      voteMap.clear();
      ongoingGamble = true;
      currentStake = arg2;
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
      std::string wagerOption, wagerAmountStr;
      ss >> wagerOption >> wagerAmountStr;

      if (!wagerOption.empty() && voteMap.find(wagerOption) != voteMap.end())
      {
        int wagerAmount = std::stoi(wagerAmountStr);
        if (mpcPointHandler->getPoints(event.msg.author.id) >= wagerAmount)
        {
          voteMap[wagerOption].push_back(event.msg.author.id);
          mpcPointHandler->updatePoints(event.msg.author.id, -wagerAmount);
          gambleAmount += wagerAmount;
          lcResponse << "I've accepted " << event.msg.author.username << "'s wager of " << wagerAmount << " points for " << wagerOption << ".";
        }
        else
        {
          lcResponse << event.msg.author.username << " does not have enough points to wager that amount.";
        }
      }
      else
      {
        lcResponse << "Invalid wager option. Please choose one of the options provided in the question.";
      }
    }
  }

  else if (arg1 == "results")
  {
    if (ongoingGamble)
    {
      if (!arg2.empty() && voteMap.find(arg2) != voteMap.end())
      {
        std::vector<dpp::snowflake>& winners = voteMap[arg2];
        if (!winners.empty())
        {
          int prize = gambleAmount / winners.size();
          for (dpp::snowflake winner : winners)
          {
            mpcPointHandler->updatePoints(winner, prize);
          }
          lcResponse << "Congratulations to the winners for earning " << prize << " points!";
          voteMap.clear();
          gambleAmount = 0;
          ongoingGamble = false;
        }
        else
        {
          lcResponse << "No one voted for " << arg2 << ".";
        }
      }
      else
      {
        lcResponse << "There is no gamble in progress.";
      }
    }
  }

  else if (arg1 == "cancel")
  {
    if (ongoingGamble)
    {
      voteMap.clear();
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
      lcResponse << "The current question is: " << currentStake;
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

  dpp::message response = dpp::message()
    .set_content(lcResponse.str())
    .set_channel_id(event.msg.channel_id);
  bot->message_create(response);
}