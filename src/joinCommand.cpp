#include <dpp/dpp.h>
#include <joinCommand.hpp>

#include <chrono>
#include <thread>

void joinCommand::registerCommand(std::shared_ptr<dpp::cluster> bot)
{
  dpp::slashcommand joincommand("join", "I will join your current voice channel", bot->me.id);
  bot->global_command_create(joincommand);
}

void joinCommand::commandCallBack(std::string command, const dpp::message_create_t& event)
{

  if (command == "!join")
  {
    this->executeCommand(event);
  }

}

void joinCommand::executeCommand(const dpp::message_create_t& event)
{
  dpp::message response_message;
  dpp::guild* g = dpp::find_guild(event.msg.guild_id);
  if (!g->connect_member_voice(event.msg.author.id))
  {
    response_message.content="You need to be in a voice channel";
  }
  else
  {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    event.from->disconnect_voice(event.msg.guild_id);
    response_message.content="I have joined (and left) the voice channel";
  }
  event.reply(response_message);
}
