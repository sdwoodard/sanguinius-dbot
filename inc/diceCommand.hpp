#ifndef DICECOMMAND_HPP_
#define DICECOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class diceCommand : public ICommand
{
public:

  diceCommand() = default;

  ~diceCommand() = default;

  void registerCommand(std::shared_ptr<dpp::cluster> bot) override;

  void commandCallBack(std::string command, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* DICECOMMAND_HPP_ */
