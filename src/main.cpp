#include <tcFileUtilities.hpp>
#include <tcMessageHandler.hpp>

#include <dpp/dpp.h>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <ctime>
#include <string>
#include <sstream>
#include <vector>

// path to file containing token on dedicated host
static std::string TOKEN_ID_FILE = "/home/sigmar/.secrets/bot.token";

int main()
{
  // set the logger filename and format
  boost::posix_time::ptime lcNow = boost::posix_time::second_clock::local_time();
  std::string lcTimeStamp = boost::posix_time::to_iso_string(lcNow);
  std::string lcLogFile = "/home/sigmar/git/sanguinius-dbot/logs/sanguinius_" + lcTimeStamp + ".log";
  boost::log::add_file_log(
    boost::log::keywords::file_name = lcLogFile,
    boost::log::keywords::format = "[%TimeStamp%] %Message%",
    boost::log::keywords::auto_flush = true
  );

  // set the logger severity level
  boost::log::core::get()->set_filter(
    boost::log::trivial::severity >= boost::log::trivial::info
  );

  // add common attributes and instantiate logger
  boost::log::add_common_attributes();
  boost::log::sources::severity_logger<boost::log::trivial::severity_level> lcLogger;

  // get bot token
  std::string botToken=tcFileUtilities::ReadStringFromFile(TOKEN_ID_FILE);

  // instantiate bot
  std::unique_ptr<dpp::cluster> lpcBot = std::make_unique<dpp::cluster>(botToken, dpp::i_default_intents | dpp::i_message_content);

  // log activities to console
  lpcBot->on_log(dpp::utility::cout_logger());

  // create objects that will handle commands
  std::unique_ptr<tcMessageHandler> lpcMsgHandler = std::make_unique<tcMessageHandler>(lpcBot.get(), &lcLogger);

  lpcBot->on_message_create([&](const dpp::message_create_t& arcEvent)
  {
    lpcMsgHandler->HandleMessage(arcEvent);
  });

  lpcBot->start(false);

  return EXIT_SUCCESS;
}
