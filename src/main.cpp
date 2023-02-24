#include <fileUtilities.hpp>
#include <messageHandler.hpp>
#include <eventRecorder.hpp>
#include <ICommand.hpp>

#include <dpp/dpp.h>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/trivial.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <ctime>
#include <string>
#include <sstream>
#include <vector>

// path to file containing token on dedicated host
static std::string TOKEN_ID_FILE = "/home/sigmar/.secrets/bot.token";

int main()
{
  // start the logger
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::string timeStamp = boost::posix_time::to_iso_string(now);
  std::string logFile = "/home/sigmar/git/sanguinius-dbot/logs/sanguinius_" + timeStamp + ".log";
  boost::log::add_file_log(
    boost::log::keywords::file_name = logFile,
    boost::log::keywords::format = "[%TimeStamp%]: %Message%"
  );

  // get bot token
  std::string botToken=fileUtilities::readStringFromFile(TOKEN_ID_FILE);

  // instantiate bot
  std::unique_ptr<dpp::cluster> bot = std::make_unique<dpp::cluster>(botToken, dpp::i_default_intents | dpp::i_message_content);

  // log activities to console
  bot->on_log(dpp::utility::cout_logger());

  std::unique_ptr<eventRecorder> eventRecords = std::make_unique<eventRecorder>();

  // create objects that will handle commands
  std::unique_ptr<messageHandler> msgHandler = std::make_unique<messageHandler>(bot.get(), eventRecords.get());

  bot->on_message_create([&](const dpp::message_create_t& event)
  {
    msgHandler->handleMessage(event);
  });

  bot->start(false);
}
