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
  dpp::message response_message;
  response_message.content="";
  dpp::guild* g = dpp::find_guild(event.command.msg.guild_id);
  auto currentVC = event.from->get_voice(event.command.msg.guild_id);
  bool joinVC = true;

  // check if the bot is already on a voice channel
  if (currentVC)
  {
    // gets channel id of user
    auto usersVC = g->voice_members.find(event.command.msg.author.id);

    // see if we are already on that channel
    if (usersVC != g->voice_members.end() && currentVC->channel_id == usersVC->second.channel_id)
    {
      // we are already there
      response_message.content="I am already with you.";
      joinVC = false;
    }

    // we are on another channel.
    // disconnect from current channel so we can join requested channel soon
    else
    {
      event.from->disconnect_voice(event.command.msg.guild_id);
      joinVC = true;
    }

    // connect to voice channel
    if (joinVC)
    {
      // attempt to connect to voice (will fall through if user is not in a channel)
      if (!g->connect_member_voice(event.command.msg.author.id))
      {
        response_message.content="You aren't in a voice channel.";
      }

      // now connect to the voice channel
      else
      {
	// we are in the channel
	// wait for on_voice_ready event to begin playing audio

	// example:
	// event.voice_client->send_audio_raw(...);

	// for now just sleep and then disconnect
	std::this_thread::sleep_for(std::chrono::seconds(10));
	event.from->disconnect_voice(event.command.msg.guild_id);

	response_message.content="I have joined (and left) your voice channel";

      }
    }
  }

  event.reply(response_message);
}
