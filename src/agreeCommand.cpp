#include <dpp/dpp.h>
#include <agreeCommand.hpp>

agreeCommand::agreeCommand(dpp::cluster* acBot)
:
  bot(acBot)
{

}

bool agreeCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event, dpp::message& oldMsg)
{
  std::cout << "Inside agreeCommand CallBack" << std::endl;

  if (keyword == "!agree")
  {
    this->executeCommand(event, oldMsg);
    return true;
  }
  else
  {
    return false;
  }

}

void agreeCommand::executeCommand(const dpp::message_create_t& event, dpp::message& oldMsg)
{
  std::cout << "Inside agreeCommand executeCommand" << std::endl;

  bot->message_delete(event.msg.id, event.msg.channel_id);
  
  dpp::message response = dpp::message()
    .set_content("Yo, Nyoo is the goat.")
    .set_type(dpp::mt_reply)
    .set_reference(oldMsg.id)
    .set_channel_id(oldMsg.channel_id);
  bot->message_create(response);

}
