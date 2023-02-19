#include <dpp/dpp.h>
#include <fileUtilities.hpp>
#include <messageHandler.hpp>
#include <ICommand.hpp>

#include <ctime>
#include <string>
#include <sstream>
#include <vector>

// path to file containing token on dedicated host
static std::string TOKEN_ID_FILE = "/home/sigmar/.secrets/bot.token";

int main()
{
  // get bot token
  std::string botToken=fileUtilities::readStringFromFile(TOKEN_ID_FILE);

  // instantiate bot
  std::unique_ptr<dpp::cluster> bot = std::make_unique<dpp::cluster>(botToken, dpp::i_default_intents | dpp::i_message_content);

  // log activities to console
  bot->on_log(dpp::utility::cout_logger());

  // create objects that will handle commands
  std::unique_ptr<messageHandler> msgHandler = std::make_unique<messageHandler>(bot.get());

  bot->on_message_create([&](const dpp::message_create_t& event)
  {
    msgHandler->handleMessage(event);
  });

  bot->start(false);
}
