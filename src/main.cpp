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

int main()
{
  // get bot token
  std::string botToken=fileUtilities::readStringFromFile(TOKEN_ID_FILE);

  // instantiate bot
  std::shared_ptr<dpp::cluster> bot = std::make_shared<dpp::cluster>(botToken, dpp::i_default_intents | dpp::i_message_content);

  // log activities to console
  bot->on_log(dpp::utility::cout_logger());

  // create objects that will handle commands
  std::shared_ptr<helpCommand> helpCommander = std::make_shared<helpCommand>();
  std::shared_ptr<dateCommand> dateCommander = std::make_shared<dateCommand>();
  std::shared_ptr<repoCommand> repoCommander = std::make_shared<repoCommand>();
  std::shared_ptr<voteCommand> voteCommander = std::make_shared<voteCommand>();
  std::shared_ptr<joinCommand> joinCommander = std::make_shared<joinCommand>();
  std::shared_ptr<diceCommand> diceCommander = std::make_shared<diceCommand>();

  // register for help command
  bot->on_ready([&](const dpp::ready_t& event)
  {
    // register all call backs for command handlers
    if (dpp::run_once<struct register_bot_commands>())
    {

      helpCommander->registerCommand(bot);
      dateCommander->registerCommand(bot);
      repoCommander->registerCommand(bot);
      voteCommander->registerCommand(bot);


    }
  });

  // command has been executed, send it to each handler
  bot->on_slashcommand([&](const dpp::slashcommand_t& event)
  {

    helpCommander->commandCallBack(event.command.get_command_name(), event);
    dateCommander->commandCallBack(event.command.get_command_name(), event);
    repoCommander->commandCallBack(event.command.get_command_name(), event);
    voteCommander->commandCallBack(event.command.get_command_name(), event);

  });

  bot->on_message_create([&](const dpp::message_create_t& event)
  {
    std::stringstream ss(event.msg.content);
    std::string command;
    ss >> command;
    joinCommander->commandCallBack(event.msg.content, event);
    diceCommander->commandCallBack(event.msg.content, event);

  });

  bot->start(false);
}
