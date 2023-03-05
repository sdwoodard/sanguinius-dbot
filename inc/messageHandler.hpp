#ifndef MESSAGEHANDLER_HPP_
#define MESSAGEHANDLER_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>
#include <helpCommand.hpp>
#include <dateCommand.hpp>
#include <repoCommand.hpp>
#include <agreeCommand.hpp>
#include <joinCommand.hpp>
#include <rollCommand.hpp>
#include <eventRecorder.hpp>
#include <pointsCommand.hpp>
#include <pointHandler.hpp>

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>

#include <vector>

class messageHandler
{
public:

  messageHandler(dpp::cluster* acBot, eventRecorder* acEventRecords, pointHandler* acPointHandler,
    boost::log::sources::severity_logger<boost::log::trivial::severity_level> acLogger);

  ~messageHandler() = default;

  void handleMessage(const dpp::message_create_t& event);

private:

  dpp::cluster* bot;

  eventRecorder* eventRecords;

  pointHandler* mpcPointHandler;

  std::unique_ptr<helpCommand> helpHandler = nullptr;
  std::unique_ptr<dateCommand> dateHandler = nullptr;
  std::unique_ptr<repoCommand> repoHandler = nullptr;
  std::unique_ptr<agreeCommand> agreeHandler = nullptr;
  std::unique_ptr<joinCommand> joinHandler = nullptr;
  std::unique_ptr<rollCommand> rollHandler = nullptr;
  std::unique_ptr<pointsCommand> mpcPointsCommand = nullptr;

  std::vector<ICommand*> msgHandlers;

  boost::log::sources::severity_logger<boost::log::trivial::severity_level> logger;

};

#endif /* MESSAGEHANDLER_HPP_*/
