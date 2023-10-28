#include <tcMessageHandler.hpp>

#include <dpp/dpp.h>
#include <boost/log/trivial.hpp>

tcMessageHandler::tcMessageHandler(dpp::cluster* apcBot, boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger)
:
  mpcBot(apcBot),
  mpcLogger(apcLogger)
{
  mcMsgHandlers["!help"] = std::make_unique<tcHelpCommand>();
  mcMsgHandlers["!repo"] = std::make_unique<tcRepoCommand>();
  mcMsgHandlers["!join"] = std::make_unique<tcJoinCommand>();
  mcMsgHandlers["!gpt"] = std::make_unique<tcGptCommand>(apcBot);

}

void tcMessageHandler::HandleMessage(const dpp::message_create_t& arcEvent)
{
  BOOST_LOG_SEV(*mpcLogger, boost::log::trivial::info) << arcEvent.msg.author.username << ": " << arcEvent.msg.content;

  if (arcEvent.msg.content.starts_with('!'))
  {
    std::string lcKeyword;
    std::size_t lnSpaceLoc = arcEvent.msg.content.find(' ');
    if (lnSpaceLoc == std::string::npos)
    {
      lcKeyword = arcEvent.msg.content;
    }
    else
    {
      lcKeyword = arcEvent.msg.content.substr(0, lnSpaceLoc);
    }

    auto lcIter = mcMsgHandlers.find(lcKeyword);
    if (lcIter != mcMsgHandlers.end())
    {
      lcIter->second->CommandCallBack(arcEvent);
    }
  }
}
