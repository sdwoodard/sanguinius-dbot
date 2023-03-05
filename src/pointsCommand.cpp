#include <dpp/dpp.h>
#include <pointsCommand.hpp>
#include <pointHandler.hpp>

pointsCommand::pointsCommand(pointHandler* acPointHandler, boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger)
: mpcPointHandler(acPointHandler),
  mpcLogger(apcLogger)
{

}

bool pointsCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!points")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void pointsCommand::executeCommand(const dpp::message_create_t& event)
{
   std::stringstream lcMessage;
   lcMessage << event.msg.author.username << " has " << mpcPointHandler->getPoints(event.msg.author.id) << " points.";

   dpp::message response_message;
   response_message.content = lcMessage.str();
   event.reply(response_message);

}
