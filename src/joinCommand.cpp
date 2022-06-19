#include <dpp/dpp.h>
#include <joinCommand.hpp>

#include <chrono>
#include <thread>

void joinCommand::registerCommand(std::shared_ptr<dpp::cluster> bot)
{
  dpp::slashcommand joincommand("join", "I will join your current voice channel", bot->me.id);
  bot->global_command_create(joincommand);
}

void joinCommand::commandCallBack(std::string command, const dpp::slashcommand_t& event)
{

  if (command == "join")
  {
    this->executeCommand(event);
  }

}

void joinCommand::executeCommand(const dpp::slashcommand_t& event)
{
  // get the voice channel information from user who sent the command
  dpp::guild* g = dpp::find_guild(event.command.msg.guild_id);
  if (!g->connect_member_voice(event.command.msg.author.id))
  {
    bot->message_create(dpp::message(event.command.msg.channel_id, "You need to be in a voice channel"));
  }


}
