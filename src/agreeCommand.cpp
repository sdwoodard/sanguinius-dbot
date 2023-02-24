#include <dpp/dpp.h>
#include <agreeCommand.hpp>

agreeCommand::agreeCommand(dpp::cluster* acBot, eventRecorder* acEventRecords)
:
  bot(acBot),
  eventRecords(acEventRecords)
{
  agreeStatements.push_back("Yo, hard agree on this one boys.");
  agreeStatements.push_back("I have never agreed harder with anything than this right here.");
  agreeStatements.push_back("This right here is something absolutely beautiful.");
  agreeStatements.push_back("My man right here is spitting straight facts.");
  agreeStatements.push_back("Absolutely! You couldn't be more right.");
  agreeStatements.push_back("Yes, yes, yes! I totally agree with you.");
  agreeStatements.push_back("That's spot on! I couldn't agree more.");
  agreeStatements.push_back("I'm with you all the way. Your point is right on the money.");
  agreeStatements.push_back("Boys. This right here is something you need to listen to.");
}

bool agreeCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{
  if (keyword == "!agree")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void agreeCommand::executeCommand(const dpp::message_create_t& event)
{
  bot->message_delete(event.msg.id, event.msg.channel_id);
  
  dpp::message userMsg;
  bool foundMessage = false;
  int index = 0;
  while (!foundMessage && index < eventRecords->maxRecords)
  {
    userMsg = eventRecords->getRecord(index).msg;
    if (userMsg.author.id == event.msg.author.id && userMsg.id != event.msg.id)
    {
      foundMessage = true;
    }

    index++;
  }

  if (foundMessage)
  {
    dpp::message response = dpp::message()
      .set_content(getAgreeStatement())
      .set_type(dpp::mt_reply)
      .set_reference(userMsg.id)
      .set_channel_id(userMsg.channel_id);
    bot->message_create(response);
  }
  else
  {
    // Message not found
  }

}

std::string agreeCommand::getAgreeStatement(void)
{
  std::srand(time(0));
  int index = rand() % agreeStatements.size();
  return agreeStatements[index];
}