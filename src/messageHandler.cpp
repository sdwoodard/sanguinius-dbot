#include <messageHandler.hpp>

#include <dpp/dpp.h>
#include <boost/log/trivial.hpp>

messageHandler::messageHandler(dpp::cluster* acBot, eventRecorder* acEventRecords)
:
  bot(acBot),
  eventRecords(acEventRecords)
{
  helpHandler = std::make_unique<helpCommand>();
  dateHandler = std::make_unique<dateCommand>();
  repoHandler = std::make_unique<repoCommand>();
  agreeHandler = std::make_unique<agreeCommand>(acBot, acEventRecords);
  joinHandler = std::make_unique<joinCommand>();
  rollHandler = std::make_unique<rollCommand>();

  msgHandlers.push_back(helpHandler.get());
  msgHandlers.push_back(dateHandler.get());
  msgHandlers.push_back(repoHandler.get());
  msgHandlers.push_back(agreeHandler.get());
  msgHandlers.push_back(joinHandler.get());
  msgHandlers.push_back(rollHandler.get());
}

void messageHandler::handleMessage(const dpp::message_create_t& event)
{
  BOOST_LOG_TRIVIAL(info) << event.msg.author.username << ": " << event.msg.content;

  eventRecords->addRecord(event);

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
