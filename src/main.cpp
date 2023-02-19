#include <dpp/dpp.h>
#include <fileUtilities.hpp>
#include <ICommand.hpp>
#include <helpCommand.hpp>
#include <dateCommand.hpp>
#include <repoCommand.hpp>
#include <voteCommand.hpp>
#include <joinCommand.hpp>
#include <diceCommand.hpp>

#include <ctime>
#include <string>
#include <sstream>
#include <vector>

// path to file containing token on dedicated host
static std::string TOKEN_ID_FILE = "/home/sigmar/.secrets/bot.token";

void handleMessage(const dpp::message_create_t& event)
{
    std::stringstream ss(event.msg.content);
    repoHandler->commandCallBack(event.msg.content, event);
    voteHandler->commandCallBack(event.msg.content, event);
    dateHandler->commandCallBack(event.msg.content, event);
    joinHandler->commandCallBack(event.msg.content, event);
    diceHandler->commandCallBack(event.msg.content, event);
}

int main()
{
  // get bot token
  std::string botToken=fileUtilities::readStringFromFile(TOKEN_ID_FILE);

  // instantiate bot
  std::unique_ptr<dpp::cluster> bot = std::make_unique<dpp::cluster>(botToken, dpp::i_default_intents | dpp::i_message_content);

  // log activities to console
  bot->on_log(dpp::utility::cout_logger());

  // create objects that will handle commands
  std::unique_ptr<helpCommand> helpHandler = std::make_unique<helpCommand>();
  std::unique_ptr<dateCommand> dateHandler = std::make_unique<dateCommand>();
  std::unique_ptr<repoCommand> repoHandler = std::make_unique<repoCommand>();
  std::unique_ptr<voteCommand> voteHandler = std::make_unique<voteCommand>();
  std::unique_ptr<joinCommand> joinHandler = std::make_unique<joinCommand>();
  std::unique_ptr<diceCommand> diceHandler = std::make_unique<diceCommand>();

  // register for help command
  bot->on_ready([&](const dpp::ready_t& event)
  {
    // register all call backs for command handlers
    if (dpp::run_once<struct register_bot_commands>())
    {
      helpHandler->registerCommand(bot.get());

    }
  });

  // command has been executed, send it to each handler
  bot->on_slashcommand([&](const dpp::slashcommand_t& event)
  {
    helpHandler->commandCallBack(event.command.get_command_name(), event);
  });

  bot->on_message_create([&](const dpp::message_create_t& event)
  {
    handleMessage(event);
  });

  bot->start(false);
}
