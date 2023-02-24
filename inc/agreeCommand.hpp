#ifndef AGREECOMMAND_HPP_
#define AGREECOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>
#include <eventRecorder.hpp>

class agreeCommand : public ICommand
{
public:

  agreeCommand(dpp::cluster* acBot, eventRecorder* acEventRecords);

  ~agreeCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event);

private:

  void executeCommand(const dpp::message_create_t& event);

  std::string getAgreeStatement(void);

  dpp::cluster* bot;

  eventRecorder* eventRecords;

  std::vector<std::string> agreeStatements;

};

#endif /* AGREECOMMAND_HPP_ */
