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

#include <vector>

class messageHandler
{
public:

  messageHandler(dpp::cluster* acBot, eventRecorder* acEventRecords);

  ~messageHandler() = default;

  void handleMessage(const dpp::message_create_t& event);

private:

  dpp::cluster* bot;

  eventRecorder* eventRecords;

  std::unique_ptr<helpCommand> helpHandler = nullptr;
  std::unique_ptr<dateCommand> dateHandler = nullptr;
  std::unique_ptr<repoCommand> repoHandler = nullptr;
  std::unique_ptr<agreeCommand> agreeHandler = nullptr;
  std::unique_ptr<joinCommand> joinHandler = nullptr;
  std::unique_ptr<rollCommand> rollHandler = nullptr;

  std::vector<ICommand*> msgHandlers;

};

#endif /* MESSAGEHANDLER_HPP_*/
