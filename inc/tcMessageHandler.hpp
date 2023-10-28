#ifndef TCMESSAGEHANDLER_HPP_
#define TCMESSAGEHANDLER_HPP_

#include <dpp/dpp.h>
#include <tcCommand.hpp>
#include <tcHelpCommand.hpp>
#include <tcRepoCommand.hpp>
#include <tcJoinCommand.hpp>
#include <tcGptCommand.hpp>

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>

#include <unordered_map>

class tcMessageHandler
{
public:

  tcMessageHandler(dpp::cluster* acBot, boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger);

  ~tcMessageHandler() = default;

  void HandleMessage(const dpp::message_create_t& arcEvent);

private:

  dpp::cluster* mpcBot;

  std::unordered_map<std::string, std::unique_ptr<tcCommand>> mcMsgHandlers;

  boost::log::sources::severity_logger<boost::log::trivial::severity_level>* mpcLogger;

};

#endif /* TCMESSAGEHANDLER_HPP_*/
