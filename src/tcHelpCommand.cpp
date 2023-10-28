#include <dpp/dpp.h>
#include <tcHelpCommand.hpp>

void tcHelpCommand::CommandCallBack(const dpp::message_create_t& arcEvent)
{
  dpp::message lcBotResponse;
  std::string lcBotResponseContent;
  lcBotResponseContent = "Sanguinius supports the following commands:\n";
  lcBotResponseContent += "    !gpt   - Ask me any question and I will respond.\n";
  lcBotResponseContent += "    !join  - Command me to join the voice channel you're currently in\n";
  lcBotResponseContent += "    !repo  - Link the GitHub public repo for this bot\n";

  lcBotResponse.content=lcBotResponseContent;
  arcEvent.reply(lcBotResponse);

}
