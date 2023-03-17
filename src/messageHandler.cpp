#include <messageHandler.hpp>

#include <dpp/dpp.h>
#include <boost/log/trivial.hpp>

messageHandler::messageHandler(dpp::cluster* acBot, eventRecorder* acEventRecords, pointHandler* acPointHandler,
  boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger)
:
  bot(acBot),
  eventRecords(acEventRecords),
  mpcPointHandler(acPointHandler),
  logger(apcLogger)
{
  helpHandler = std::make_unique<helpCommand>();
  dateHandler = std::make_unique<dateCommand>();
  repoHandler = std::make_unique<repoCommand>();
  agreeHandler = std::make_unique<agreeCommand>(acBot, acEventRecords);
  joinHandler = std::make_unique<joinCommand>();
  rollHandler = std::make_unique<rollCommand>();
  mpcPointsCommand = std::make_unique<pointsCommand>(mpcPointHandler, apcLogger);
  mpcGambleCommand = std::make_unique<gambleCommand>(acBot, mpcPointHandler, apcLogger);
  mpcGptCommand = std::make_unique<gptCommand>();

  msgHandlers.push_back(helpHandler.get());
  msgHandlers.push_back(dateHandler.get());
  msgHandlers.push_back(repoHandler.get());
  msgHandlers.push_back(agreeHandler.get());
  msgHandlers.push_back(joinHandler.get());
  msgHandlers.push_back(rollHandler.get());
  msgHandlers.push_back(mpcPointsCommand.get());
  msgHandlers.push_back(mpcGambleCommand.get());
  msgHandlers.push_back(mpcGptCommand.get());
}

void messageHandler::handleMessage(const dpp::message_create_t& event)
{
  BOOST_LOG_SEV(*logger, boost::log::trivial::info) << event.msg.author.username << ": " << event.msg.content;

  eventRecords->addRecord(event);
  mpcPointHandler->updatePoints(event.msg.author.id, 1);

  if (event.msg.content[0] == '!')
  {
    std::string keyword;
    std::size_t spaceLoc = event.msg.content.find(' ');
    if (spaceLoc == std::string::npos)
    {
      keyword = event.msg.content;
    }
    else
    {
      keyword = event.msg.content.substr(0, spaceLoc);
    }

    bool msgHandled = false;
    for (auto handler : msgHandlers)
    {
      if (!msgHandled)
      {
        msgHandled = handler->commandCallBack(keyword, event);
      }
      else
      {
        break;
      }
    }
  }

}
