#include <dpp/dpp.h>
#include <messageHandler.hpp>

messageHandler::messageHandler(dpp::cluster* acBot)
:
  bot(acBot)
{
  helpHandler = std::make_unique<helpCommand>();
  dateHandler = std::make_unique<dateCommand>();
  repoHandler = std::make_unique<repoCommand>();
  voteHandler = std::make_unique<voteCommand>();
  joinHandler = std::make_unique<joinCommand>();
  rollHandler = std::make_unique<rollCommand>();

  msgHandlers.push_back(helpHandler.get());
  msgHandlers.push_back(dateHandler.get());
  msgHandlers.push_back(repoHandler.get());
  msgHandlers.push_back(voteHandler.get());
  msgHandlers.push_back(joinHandler.get());
  msgHandlers.push_back(rollHandler.get());
}

void messageHandler::handleMessage(const dpp::message_create_t& event)
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

  std::cout << "CHAT: " << event.msg.content << std::endl;

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
