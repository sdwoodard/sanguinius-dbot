#include <dpp/dpp.h>
#include <tcJoinCommand.hpp>

#include <chrono>
#include <thread>

void tcJoinCommand::CommandCallBack(const dpp::message_create_t& arcEvent)
{
  dpp::message lcBotResponse;
  dpp::guild* lpcGuild = dpp::find_guild(arcEvent.msg.guild_id);
  if (!lpcGuild->connect_member_voice(arcEvent.msg.author.id))
  {
    lcBotResponse.content="You need to be in a voice channel";
  }
  else
  {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    arcEvent.from->disconnect_voice(arcEvent.msg.guild_id);
    lcBotResponse.content="I have joined (and left) the voice channel";
  }
  arcEvent.reply(lcBotResponse);
}
