#include <dpp/dpp.h>
#include <pointsCommand.hpp>
#include <pointHandler.hpp>

pointsCommand::pointsCommand(pointHandler* acPointHandler)
: mpcPointHandler(acPointHandler)
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

   dpp::message lcResponse = dpp::message()
      .set_content(lcMessage.str())
      .set_type(dpp::mt_reply)
      .set_reference(event.msg.id)
      .set_channel_id(event.msg.channel_id);
   bot->message_create(lcResponse);

}
